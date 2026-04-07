/*
 * FlexQL: Durable append-only log implementation.
 */

#include "durable_log.h"

#include "parser.h"
#include "storage.h"

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr char kLogMagic[] = "FXQLWAL";
constexpr uint32_t kLogVersion = 1;
constexpr uint32_t kRecordMagic = 0x514c5846U;

enum RecordType : uint32_t {
    kCommittedCreateRecord = 1,
    kCommittedInsertRecord = 2,
    kCommittedDeleteRecord = 3,
    kPrepareCreateRecord = 11,
    kPrepareInsertRecord = 12,
    kPrepareDeleteRecord = 13,
    kCommitRecord = 14,
};

template <typename T>
void appendPod(std::string &buffer, const T &value) {
    const size_t offset = buffer.size();
    buffer.resize(offset + sizeof(T));
    std::memcpy(&buffer[offset], &value, sizeof(T));
}

template <typename T>
bool readPod(const std::string &buffer, size_t &offset, T &value) {
    if (offset + sizeof(T) > buffer.size()) {
        return false;
    }
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

void appendString(std::string &buffer, const std::string &value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    appendPod(buffer, size);
    buffer.append(value.data(), value.size());
}

bool readString(const std::string &buffer, size_t &offset, std::string &value) {
    uint32_t size = 0;
    if (!readPod(buffer, offset, size) || offset + size > buffer.size()) {
        return false;
    }
    value.assign(buffer.data() + offset, size);
    offset += size;
    return true;
}

bool ensureParentDir(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }

    const std::string dir = path.substr(0, slash);
    if (dir.empty()) {
        return true;
    }

    if (::mkdir(dir.c_str(), 0777) == 0) {
        return true;
    }
    return errno == EEXIST;
}

bool writeAll(int fd, const char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        const ssize_t rc = ::write(fd, buf + written, len - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(rc);
    }
    return true;
}

bool fileHeaderExists(int fd) {
    const off_t end = ::lseek(fd, 0, SEEK_END);
    return end > 0;
}

bool appendCreatePayload(const ParsedQuery &query, std::string &payload) {
    appendString(payload, query.tableName);
    const uint32_t columnCount = static_cast<uint32_t>(query.columns.size());
    appendPod(payload, columnCount);
    for (const ColumnDef &column : query.columns) {
        appendString(payload, column.name);
        const uint32_t typeValue = static_cast<uint32_t>(column.type);
        const uint8_t notNull = column.notNull ? 1 : 0;
        const uint8_t primaryKey = column.primaryKey ? 1 : 0;
        appendPod(payload, typeValue);
        appendPod(payload, notNull);
        appendPod(payload, primaryKey);
    }
    return true;
}

bool appendInsertPayload(const ParsedQuery &query, std::string &payload) {
    appendString(payload, query.tableName);
    appendPod(payload, query.expiresAt);
    const uint32_t rowCount = static_cast<uint32_t>(query.insertRows.size());
    appendPod(payload, rowCount);
    for (const auto &row : query.insertRows) {
        const uint32_t valueCount = static_cast<uint32_t>(row.size());
        appendPod(payload, valueCount);
        for (const std::string &value : row) {
            appendString(payload, value);
        }
    }
    return true;
}

bool appendDeletePayload(const ParsedQuery &query, std::string &payload) {
    appendString(payload, query.tableName);
    return true;
}

bool rowExpired(const Row &row, int64_t nowMs) {
    return row.expires_at != 0 && row.expires_at <= nowMs;
}

uint32_t committedTypeFor(uint32_t prepareType) {
    switch (prepareType) {
        case kPrepareCreateRecord:
            return kCommittedCreateRecord;
        case kPrepareInsertRecord:
            return kCommittedInsertRecord;
        case kPrepareDeleteRecord:
            return kCommittedDeleteRecord;
        default:
            return 0;
    }
}

bool decodeTxScopedPayload(const std::string &payload,
                           uint64_t &txId,
                           std::string &body) {
    size_t offset = 0;
    if (!readPod(payload, offset, txId)) {
        return false;
    }
    body.assign(payload.data() + offset, payload.size() - offset);
    return true;
}

bool applyCreateRecord(const std::string &payload,
                       StorageEngine &storage,
                       std::string &err) {
    size_t offset = 0;
    std::string tableName;
    uint32_t columnCount = 0;
    if (!readString(payload, offset, tableName) ||
        !readPod(payload, offset, columnCount)) {
        err = "Failed to decode CREATE record";
        return false;
    }

    std::vector<ColumnDef> schema;
    schema.reserve(columnCount);
    for (uint32_t i = 0; i < columnCount; ++i) {
        ColumnDef column;
        uint32_t typeValue = 0;
        uint8_t notNull = 0;
        uint8_t primaryKey = 0;
        if (!readString(payload, offset, column.name) ||
            !readPod(payload, offset, typeValue) ||
            !readPod(payload, offset, notNull) ||
            !readPod(payload, offset, primaryKey)) {
            err = "Failed to decode CREATE column";
            return false;
        }
        column.type = static_cast<ColType>(typeValue);
        column.notNull = (notNull != 0);
        column.primaryKey = (primaryKey != 0);
        schema.push_back(column);
    }

    if (!storage.createTable(tableName, schema)) {
        err = "Failed to replay CREATE TABLE for " + tableName;
        return false;
    }
    return true;
}

bool applyInsertRecord(const std::string &payload,
                       StorageEngine &storage,
                       int64_t nowMs,
                       std::string &err) {
    size_t offset = 0;
    std::string tableName;
    int64_t expiresAt = 0;
    uint32_t rowCount = 0;
    if (!readString(payload, offset, tableName) ||
        !readPod(payload, offset, expiresAt) ||
        !readPod(payload, offset, rowCount)) {
        err = "Failed to decode INSERT record";
        return false;
    }

    Table *table = storage.getTable(tableName);
    if (table == nullptr) {
        err = "INSERT record references missing table: " + tableName;
        return false;
    }

    for (uint32_t rowNo = 0; rowNo < rowCount; ++rowNo) {
        uint32_t valueCount = 0;
        if (!readPod(payload, offset, valueCount)) {
            err = "Failed to decode INSERT row";
            return false;
        }

        Row row;
        row.expires_at = expiresAt;
        row.values.reserve(valueCount);
        for (uint32_t valueNo = 0; valueNo < valueCount; ++valueNo) {
            std::string value;
            if (!readString(payload, offset, value)) {
                err = "Failed to decode INSERT value";
                return false;
            }
            row.values.push_back(std::move(value));
        }

        if (rowExpired(row, nowMs)) {
            continue;
        }
        uint64_t rowOffset = 0;
        if (!storage.insertRow(*table, row, &rowOffset)) {
            err = "Failed to replay INSERT for " + tableName;
            return false;
        }
        if (table->primaryKeyIdx >= 0) {
            storage.upsertPrimaryKey(*table, row.values[table->primaryKeyIdx], rowOffset);
        }
    }
    return true;
}

bool applyDeleteRecord(const std::string &payload,
                       StorageEngine &storage,
                       std::string &err) {
    size_t offset = 0;
    std::string tableName;
    if (!readString(payload, offset, tableName)) {
        err = "Failed to decode DELETE record";
        return false;
    }

    Table *table = storage.getTable(tableName);
    if (table == nullptr) {
        err = "DELETE record references missing table: " + tableName;
        return false;
    }
    storage.clearTable(*table);
    return true;
}

bool applyCommittedRecord(uint32_t type,
                          const std::string &payload,
                          StorageEngine &storage,
                          int64_t nowMs,
                          std::string &err) {
    switch (type) {
        case kCommittedCreateRecord:
            return applyCreateRecord(payload, storage, err);
        case kCommittedInsertRecord:
            return applyInsertRecord(payload, storage, nowMs, err);
        case kCommittedDeleteRecord:
            return applyDeleteRecord(payload, storage, err);
        default:
            err = "Unknown durable log record type";
            return false;
    }
}

} // namespace

DurableLog::DurableLog(std::string path)
    : path_(std::move(path)) {}

DurableLog::~DurableLog() {
    close();
}

bool DurableLog::open(std::string &err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        return true;
    }
    if (!ensureParentDir(path_)) {
        err = "Failed to create durable log directory";
        return false;
    }

    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0666);
    if (fd_ < 0) {
        err = "Failed to open durable log";
        return false;
    }

    if (!fileHeaderExists(fd_)) {
        if (!writeAll(fd_, kLogMagic, sizeof(kLogMagic) - 1) ||
            !writeAll(fd_, reinterpret_cast<const char *>(&kLogVersion), sizeof(kLogVersion)) ||
            ::fdatasync(fd_) != 0) {
            err = "Failed to initialize durable log";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }
    return true;
}

void DurableLog::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool DurableLog::beginCreate(const ParsedQuery &query, uint64_t &txId, std::string &err) {
    std::string payload;
    appendCreatePayload(query, payload);
    return appendRecord(kPrepareCreateRecord, txId, payload, true, true, err);
}

bool DurableLog::beginInsert(const ParsedQuery &query, uint64_t &txId, std::string &err) {
    std::string payload;
    appendInsertPayload(query, payload);
    return appendRecord(kPrepareInsertRecord, txId, payload, true, true, err);
}

bool DurableLog::beginInsertNoSync(const ParsedQuery &query, uint64_t &txId, std::string &err) {
    std::string payload;
    appendInsertPayload(query, payload);
    return appendRecord(kPrepareInsertRecord, txId, payload, true, false, err);
}

bool DurableLog::beginDelete(const ParsedQuery &query, uint64_t &txId, std::string &err) {
    std::string payload;
    appendDeletePayload(query, payload);
    return appendRecord(kPrepareDeleteRecord, txId, payload, true, true, err);
}

bool DurableLog::commit(uint64_t txId, std::string &err) {
    if (txId == 0) {
        err = "Invalid durable log transaction id";
        return false;
    }
    return appendRecord(kCommitRecord, txId, std::string(), true, true, err);
}

bool DurableLog::commitNoSync(uint64_t txId, std::string &err) {
    if (txId == 0) {
        err = "Invalid durable log transaction id";
        return false;
    }
    return appendRecord(kCommitRecord, txId, std::string(), true, false, err);
}

bool DurableLog::sync(std::string &err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        err = "Durable log is not open";
        return false;
    }
    if (::fdatasync(fd_) != 0) {
        err = "Failed to sync durable log";
        return false;
    }
    return true;
}

const std::string &DurableLog::path() const {
    return path_;
}

bool DurableLog::appendRecord(uint32_t type,
                              uint64_t &txId,
                              const std::string &payload,
                              bool includeTxIdInPayload,
                              bool doSync,
                              std::string &err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) {
        err = "Durable log is not open";
        return false;
    }

    if (includeTxIdInPayload && txId == 0) {
        const off_t end = ::lseek(fd_, 0, SEEK_END);
        if (end < 0) {
            err = "Failed to allocate durable log transaction id";
            return false;
        }
        txId = static_cast<uint64_t>(end);
    }

    std::string recordPayload;
    if (includeTxIdInPayload) {
        appendPod(recordPayload, txId);
    }
    recordPayload.append(payload);

    const uint32_t payloadSize = static_cast<uint32_t>(recordPayload.size());
    if (!writeAll(fd_, reinterpret_cast<const char *>(&kRecordMagic), sizeof(kRecordMagic)) ||
        !writeAll(fd_, reinterpret_cast<const char *>(&type), sizeof(type)) ||
        !writeAll(fd_, reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize)) ||
        (!recordPayload.empty() && !writeAll(fd_, recordPayload.data(), recordPayload.size()))) {
        err = "Failed to write durable log record";
        return false;
    }
    if (doSync && ::fdatasync(fd_) != 0) {
        err = "Failed to flush durable log";
        return false;
    }
    return true;
}

bool replayDurableLog(const std::string &path,
                      StorageEngine &storage,
                      std::string &err) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return true;
        }
        err = "Failed to open durable log for replay";
        return false;
    }

    auto cleanup = [&]() {
        ::close(fd);
    };

    char magic[sizeof(kLogMagic)] = {};
    const ssize_t magicRead = ::read(fd, magic, sizeof(kLogMagic) - 1);
    if (magicRead == 0) {
        cleanup();
        return true;
    }
    if (magicRead != static_cast<ssize_t>(sizeof(kLogMagic) - 1) ||
        std::strncmp(magic, kLogMagic, sizeof(kLogMagic) - 1) != 0) {
        cleanup();
        err = "Invalid durable log header";
        return false;
    }

    uint32_t version = 0;
    if (::read(fd, &version, sizeof(version)) != static_cast<ssize_t>(sizeof(version)) ||
        version != kLogVersion) {
        cleanup();
        err = "Unsupported durable log version";
        return false;
    }

    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    struct PendingRecord {
        uint32_t committedType = 0;
        std::string payload;
    };
    std::unordered_map<uint64_t, PendingRecord> pending;

    while (true) {
        uint32_t recordMagic = 0;
        const ssize_t headerRead = ::read(fd, &recordMagic, sizeof(recordMagic));
        if (headerRead == 0) {
            break;
        }
        if (headerRead != static_cast<ssize_t>(sizeof(recordMagic))) {
            break;
        }

        uint32_t type = 0;
        uint32_t payloadSize = 0;
        if (::read(fd, &type, sizeof(type)) != static_cast<ssize_t>(sizeof(type)) ||
            ::read(fd, &payloadSize, sizeof(payloadSize)) != static_cast<ssize_t>(sizeof(payloadSize))) {
            break;
        }
        if (recordMagic != kRecordMagic) {
            cleanup();
            err = "Corrupted durable log record";
            return false;
        }

        std::string payload(payloadSize, '\0');
        if (payloadSize > 0) {
            size_t readBytes = 0;
            while (readBytes < payloadSize) {
                const ssize_t rc = ::read(fd, &payload[readBytes], payloadSize - readBytes);
                if (rc <= 0) {
                    break;
                }
                readBytes += static_cast<size_t>(rc);
            }
            if (readBytes != payloadSize) {
                break;
            }
        }

        if (type == kCommittedCreateRecord ||
            type == kCommittedInsertRecord ||
            type == kCommittedDeleteRecord) {
            if (!applyCommittedRecord(type, payload, storage, nowMs, err)) {
                cleanup();
                return false;
            }
        } else if (type == kPrepareCreateRecord ||
                   type == kPrepareInsertRecord ||
                   type == kPrepareDeleteRecord) {
            uint64_t txId = 0;
            std::string body;
            if (!decodeTxScopedPayload(payload, txId, body)) {
                cleanup();
                err = "Failed to decode prepared durable log record";
                return false;
            }
            const uint32_t committedType = committedTypeFor(type);
            if (committedType == 0 || pending.count(txId) != 0) {
                cleanup();
                err = "Corrupted durable log transaction state";
                return false;
            }
            pending.emplace(txId, PendingRecord{committedType, std::move(body)});
        } else if (type == kCommitRecord) {
            uint64_t txId = 0;
            size_t offset = 0;
            if (!readPod(payload, offset, txId) || offset != payload.size()) {
                cleanup();
                err = "Failed to decode COMMIT record";
                return false;
            }
            const auto it = pending.find(txId);
            if (it == pending.end()) {
                cleanup();
                err = "COMMIT record references unknown transaction";
                return false;
            }
            if (!applyCommittedRecord(it->second.committedType,
                                      it->second.payload,
                                      storage,
                                      nowMs,
                                      err)) {
                cleanup();
                return false;
            }
            pending.erase(it);
        } else {
            cleanup();
            err = "Unknown durable log record type";
            return false;
        }
    }

    cleanup();
    return true;
}
