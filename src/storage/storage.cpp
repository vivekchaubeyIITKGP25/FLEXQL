/*
 * FlexQL: Storage engine implementation.
 */

#include "storage.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kRowRecordMagic = 0x52574658U;
constexpr uint64_t kPrimaryKeyIndexMagic = 0x49444B50584C4658ULL;
constexpr uint64_t kPrimaryKeyIndexVersion = 1;
constexpr uint64_t kPrimaryKeyIndexInitialCapacity = 1ULL << 16;
constexpr double kPrimaryKeyIndexLoadFactor = 0.70;

struct PrimaryKeyIndexHeader {
    uint64_t magic = kPrimaryKeyIndexMagic;
    uint64_t version = kPrimaryKeyIndexVersion;
    uint64_t capacity = 0;
    uint64_t entries = 0;
};

struct PrimaryKeyIndexSlot {
    uint64_t fingerprint = 0;
    uint64_t rowOffset = 0;
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

bool preadAll(int fd, void *buf, size_t len, off_t offset) {
    char *out = static_cast<char *>(buf);
    size_t readBytes = 0;
    while (readBytes < len) {
        const ssize_t rc = ::pread(fd, out + readBytes, len - readBytes,
                                   offset + static_cast<off_t>(readBytes));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            return false;
        }
        readBytes += static_cast<size_t>(rc);
    }
    return true;
}

bool pwriteAll(int fd, const void *buf, size_t len, off_t offset) {
    const char *in = static_cast<const char *>(buf);
    size_t written = 0;
    while (written < len) {
        const ssize_t rc = ::pwrite(fd, in + written, len - written,
                                    offset + static_cast<off_t>(written));
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

uint64_t primaryKeyFingerprint(const std::string &key) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : key) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

off_t primaryKeyIndexHeaderOffset() {
    return 0;
}

off_t primaryKeyIndexSlotOffset(uint64_t slotIndex) {
    return static_cast<off_t>(sizeof(PrimaryKeyIndexHeader) +
                              slotIndex * sizeof(PrimaryKeyIndexSlot));
}

bool ensureDirectory(const std::string &path, std::string *err) {
    if (path.empty()) {
        if (err != nullptr) {
            *err = "Storage root path is empty";
        }
        return false;
    }

    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        if (err != nullptr) {
            *err = "Failed to create storage directory: " + path;
        }
        return false;
    }
    return true;
}

std::string makeTempRoot() {
    char tmpl[] = "/tmp/flexql-storage-XXXXXX";
    char *dir = ::mkdtemp(tmpl);
    return dir != nullptr ? std::string(dir) : std::string();
}

std::string sanitizeTableName(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char ch : name) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-') {
            out.push_back(static_cast<char>(ch));
        } else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            out.push_back('_');
            out.push_back(kHex[(ch >> 4) & 0x0F]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    if (out.empty()) {
        out = "TABLE";
    }
    return out;
}

std::string rowFilePathFor(const std::string &root, const std::string &tableName) {
    return root + "/" + sanitizeTableName(tableName) + ".rows";
}

bool encodeRowPayload(const Row &row, std::string &payload) {
    appendPod(payload, row.expires_at);
    const uint32_t valueCount = static_cast<uint32_t>(row.values.size());
    appendPod(payload, valueCount);
    for (const std::string &value : row.values) {
        appendString(payload, value);
    }
    return true;
}

bool decodeRowPayload(const std::string &payload, Row &row) {
    size_t offset = 0;
    uint32_t valueCount = 0;
    if (!readPod(payload, offset, row.expires_at) ||
        !readPod(payload, offset, valueCount)) {
        return false;
    }

    row.values.clear();
    row.values.reserve(valueCount);
    for (uint32_t i = 0; i < valueCount; ++i) {
        std::string value;
        if (!readString(payload, offset, value)) {
            return false;
        }
        row.values.push_back(std::move(value));
    }
    return offset == payload.size();
}

bool openRowFile(const std::string &path, int flags, int &fd, std::string *err) {
    fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) {
        if (err != nullptr) {
            *err = "Failed to open row file: " + path;
        }
        return false;
    }
    return true;
}

void closeRowFile(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void closePrimaryKeyIndex(Table &tbl) {
    if (tbl.primaryKeyIndexFd >= 0) {
        ::close(tbl.primaryKeyIndexFd);
        tbl.primaryKeyIndexFd = -1;
    }
}

void closeReadFile(Table &tbl) {
    if (tbl.readFileFd >= 0) {
        ::close(tbl.readFileFd);
        tbl.readFileFd = -1;
    }
}

void resetAppendState(Table &tbl) {
    closeRowFile(tbl.rowFileFd);
    closeReadFile(tbl);
    tbl.rowFileEndOffset = 0;
}

bool ensureReadHandle(Table &tbl, std::string *err) {
    if (tbl.readFileFd >= 0) {
        return true;
    }
    int fd = -1;
    if (!openRowFile(tbl.rowFilePath, O_RDONLY, fd, err)) {
        return false;
    }
    tbl.readFileFd = fd;
    return true;
}

void resetPrimaryKeyIndexState(Table &tbl) {
    closePrimaryKeyIndex(tbl);
    tbl.primaryKeyIndexCapacity = 0;
    tbl.primaryKeyIndexEntries = 0;
    tbl.primaryKeyIndexAvailable = false;
}

bool ensureAppendHandle(Table &tbl, std::string *err) {
    if (tbl.rowFileFd >= 0) {
        return true;
    }

    int fd = -1;
    if (!openRowFile(tbl.rowFilePath, O_CREAT | O_WRONLY | O_APPEND, fd, err)) {
        return false;
    }

    const off_t currentEnd = ::lseek(fd, 0, SEEK_END);
    if (currentEnd < 0) {
        if (err != nullptr) {
            *err = "Failed to seek row file";
        }
        closeRowFile(fd);
        return false;
    }

    tbl.rowFileFd = fd;
    tbl.rowFileEndOffset = static_cast<uint64_t>(currentEnd);
    return true;
}

bool ensurePrimaryKeyIndexHandle(Table &tbl, std::string *err) {
    if (tbl.primaryKeyIdx < 0) {
        return true;
    }
    if (!tbl.primaryKeyIndexAvailable) {
        if (err != nullptr) {
            *err = "Primary key index is unavailable";
        }
        return false;
    }
    if (tbl.primaryKeyIndexFd >= 0) {
        return true;
    }

    int fd = ::open(tbl.primaryKeyIndexPath.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        tbl.primaryKeyIndexAvailable = false;
        if (err != nullptr) {
            *err = "Failed to open primary key index: " + tbl.primaryKeyIndexPath;
        }
        return false;
    }

    tbl.primaryKeyIndexFd = fd;
    return true;
}

bool writePrimaryKeyIndexHeader(const Table &tbl, std::string *err) {
    PrimaryKeyIndexHeader header;
    header.capacity = tbl.primaryKeyIndexCapacity;
    header.entries = tbl.primaryKeyIndexEntries;
    if (!pwriteAll(tbl.primaryKeyIndexFd, &header, sizeof(header), primaryKeyIndexHeaderOffset())) {
        if (err != nullptr) {
            *err = "Failed to write primary key index header";
        }
        return false;
    }
    return true;
}

bool readPrimaryKeyIndexSlot(const Table &tbl,
                             uint64_t slotIndex,
                             PrimaryKeyIndexSlot &slot,
                             std::string *err) {
    if (!preadAll(tbl.primaryKeyIndexFd, &slot, sizeof(slot),
                  primaryKeyIndexSlotOffset(slotIndex))) {
        if (err != nullptr) {
            *err = "Failed to read primary key index slot";
        }
        return false;
    }
    return true;
}

bool writePrimaryKeyIndexSlot(const Table &tbl,
                              uint64_t slotIndex,
                              const PrimaryKeyIndexSlot &slot,
                              std::string *err) {
    if (!pwriteAll(tbl.primaryKeyIndexFd, &slot, sizeof(slot),
                   primaryKeyIndexSlotOffset(slotIndex))) {
        if (err != nullptr) {
            *err = "Failed to write primary key index slot";
        }
        return false;
    }
    return true;
}

bool resetPrimaryKeyIndex(Table &tbl, std::string *err) {
    if (tbl.primaryKeyIdx < 0) {
        resetPrimaryKeyIndexState(tbl);
        return true;
    }

    closePrimaryKeyIndex(tbl);
    tbl.primaryKeyIndexAvailable = true;
    tbl.primaryKeyIndexCapacity = kPrimaryKeyIndexInitialCapacity;
    tbl.primaryKeyIndexEntries = 0;

    int fd = ::open(tbl.primaryKeyIndexPath.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        resetPrimaryKeyIndexState(tbl);
        if (err != nullptr) {
            *err = "Failed to create primary key index: " + tbl.primaryKeyIndexPath;
        }
        return false;
    }
    tbl.primaryKeyIndexFd = fd;

    const off_t fileSize = primaryKeyIndexSlotOffset(tbl.primaryKeyIndexCapacity);
    if (::ftruncate(tbl.primaryKeyIndexFd, fileSize) != 0 || !writePrimaryKeyIndexHeader(tbl, err)) {
        resetPrimaryKeyIndexState(tbl);
        if (tbl.primaryKeyIndexFd >= 0) {
            closePrimaryKeyIndex(tbl);
        }
        return false;
    }
    return true;
}

bool insertPrimaryKeyIndexSlotRaw(int fd,
                                  uint64_t capacity,
                                  const PrimaryKeyIndexSlot &slot,
                                  std::string *err) {
    uint64_t slotIndex = slot.fingerprint % capacity;
    for (uint64_t probe = 0; probe < capacity; ++probe) {
        PrimaryKeyIndexSlot existing;
        if (!preadAll(fd, &existing, sizeof(existing), primaryKeyIndexSlotOffset(slotIndex))) {
            if (err != nullptr) {
                *err = "Failed to read primary key index slot";
            }
            return false;
        }
        if (existing.fingerprint == 0) {
            if (!pwriteAll(fd, &slot, sizeof(slot), primaryKeyIndexSlotOffset(slotIndex))) {
                if (err != nullptr) {
                    *err = "Failed to write primary key index slot";
                }
                return false;
            }
            return true;
        }
        slotIndex = (slotIndex + 1) % capacity;
    }

    if (err != nullptr) {
        *err = "Primary key index is full";
    }
    return false;
}

bool resizePrimaryKeyIndex(Table &tbl, uint64_t newCapacity, std::string *err) {
    const std::string tempPath = tbl.primaryKeyIndexPath + ".tmp";
    int tempFd = ::open(tempPath.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (tempFd < 0) {
        if (err != nullptr) {
            *err = "Failed to create temporary primary key index";
        }
        return false;
    }

    PrimaryKeyIndexHeader newHeader;
    newHeader.capacity = newCapacity;
    newHeader.entries = 0;
    const off_t fileSize = primaryKeyIndexSlotOffset(newCapacity);
    bool ok = (::ftruncate(tempFd, fileSize) == 0) &&
              pwriteAll(tempFd, &newHeader, sizeof(newHeader), primaryKeyIndexHeaderOffset());

    if (ok) {
        for (uint64_t slotIndex = 0; slotIndex < tbl.primaryKeyIndexCapacity; ++slotIndex) {
            PrimaryKeyIndexSlot slot;
            if (!readPrimaryKeyIndexSlot(tbl, slotIndex, slot, err)) {
                ok = false;
                break;
            }
            if (slot.fingerprint == 0) {
                continue;
            }
            if (!insertPrimaryKeyIndexSlotRaw(tempFd, newCapacity, slot, err)) {
                ok = false;
                break;
            }
            ++newHeader.entries;
        }
    }

    if (ok) {
        ok = pwriteAll(tempFd, &newHeader, sizeof(newHeader), primaryKeyIndexHeaderOffset());
        if (!ok && err != nullptr) {
            *err = "Failed to finalize primary key index resize";
        }
    }

    ::close(tempFd);

    if (!ok) {
        std::remove(tempPath.c_str());
        return false;
    }

    closePrimaryKeyIndex(tbl);
    if (std::rename(tempPath.c_str(), tbl.primaryKeyIndexPath.c_str()) != 0) {
        if (err != nullptr) {
            *err = "Failed to replace primary key index";
        }
        std::remove(tempPath.c_str());
        return false;
    }

    tbl.primaryKeyIndexCapacity = newCapacity;
    tbl.primaryKeyIndexEntries = newHeader.entries;
    return ensurePrimaryKeyIndexHandle(tbl, err) && writePrimaryKeyIndexHeader(tbl, err);
}

bool maybeGrowPrimaryKeyIndex(Table &tbl, std::string *err) {
    if (tbl.primaryKeyIdx < 0 || !tbl.primaryKeyIndexAvailable) {
        return true;
    }
    if (tbl.primaryKeyIndexCapacity == 0) {
        return resetPrimaryKeyIndex(tbl, err);
    }
    const double threshold =
        static_cast<double>(tbl.primaryKeyIndexCapacity) * kPrimaryKeyIndexLoadFactor;
    if (static_cast<double>(tbl.primaryKeyIndexEntries + 1) <= threshold) {
        return true;
    }
    return resizePrimaryKeyIndex(tbl, tbl.primaryKeyIndexCapacity * 2, err);
}

bool readRowRecord(int fd,
                   uint64_t offset,
                   Row &row,
                   bool &eof,
                   uint64_t *nextOffset,
                   std::string *err) {
    eof = false;

    uint32_t recordMagic = 0;
    const ssize_t magicRead = ::pread(fd, &recordMagic, sizeof(recordMagic),
                                      static_cast<off_t>(offset));
    if (magicRead == 0) {
        eof = true;
        return true;
    }
    if (magicRead != static_cast<ssize_t>(sizeof(recordMagic))) {
        if (err != nullptr) {
            *err = "Failed to read row record header";
        }
        return false;
    }

    uint32_t payloadSize = 0;
    if (::pread(fd, &payloadSize, sizeof(payloadSize),
                static_cast<off_t>(offset + sizeof(recordMagic))) !=
        static_cast<ssize_t>(sizeof(payloadSize))) {
        if (err != nullptr) {
            *err = "Failed to read row payload size";
        }
        return false;
    }

    if (recordMagic != kRowRecordMagic) {
        if (err != nullptr) {
            *err = "Corrupted row file record";
        }
        return false;
    }

    std::string payload(payloadSize, '\0');
    if (payloadSize > 0 &&
        ::pread(fd, &payload[0], payloadSize,
                static_cast<off_t>(offset + sizeof(recordMagic) + sizeof(payloadSize))) !=
            static_cast<ssize_t>(payloadSize)) {
        if (err != nullptr) {
            *err = "Failed to read row payload";
        }
        return false;
    }

    if (!decodeRowPayload(payload, row)) {
        if (err != nullptr) {
            *err = "Failed to decode row payload";
        }
        return false;
    }
    if (nextOffset != nullptr) {
        *nextOffset = offset + sizeof(recordMagic) + sizeof(payloadSize) + payloadSize;
    }
    return true;
}

} // namespace

StorageEngine::StorageEngine(std::string rootPath) {
    if (rootPath.empty()) {
        storageRoot_ = makeTempRoot();
        ownsStorageRoot_ = true;
    } else {
        storageRoot_ = std::move(rootPath);
    }

    std::string err;
    ensureDirectory(storageRoot_, &err);
}

StorageEngine::~StorageEngine() {
    for (auto &entry : tables_) {
        resetAppendState(*entry.second);
        closeReadFile(*entry.second);
        closePrimaryKeyIndex(*entry.second);
    }

    if (!ownsStorageRoot_ || storageRoot_.empty()) {
        return;
    }

    std::error_code ec;
    fs::remove_all(storageRoot_, ec);
}

bool StorageEngine::createTable(const std::string &name,
                                const std::vector<ColumnDef> &schema,
                                std::string *err) {
    std::unique_lock<std::shared_mutex> lock(catalogLock_);
    if (tables_.count(name) != 0) {
        return false;
    }

    if (!ensureDirectory(storageRoot_, err)) {
        return false;
    }

    std::unique_ptr<Table> tbl = std::make_unique<Table>();
    tbl->name = name;
    tbl->schema = schema;
    tbl->rowFilePath = rowFilePathFor(storageRoot_, name);
    tbl->primaryKeyIndexPath = tbl->rowFilePath + ".pk";
    tbl->rowCount = 0;

    for (size_t i = 0; i < schema.size(); ++i) {
        if (schema[i].primaryKey) {
            tbl->primaryKeyIdx = static_cast<int>(i);
            break;
        }
    }

    int fd = -1;
    if (!openRowFile(tbl->rowFilePath, O_CREAT | O_WRONLY | O_TRUNC, fd, err)) {
        return false;
    }
    closeRowFile(fd);

    if (tbl->primaryKeyIdx >= 0 && !resetPrimaryKeyIndex(*tbl, err)) {
        return false;
    }

    tables_[name] = std::move(tbl);
    return true;
}

bool StorageEngine::dropTable(const std::string &name, std::string *err) {
    std::unique_ptr<Table> tbl;
    {
        std::unique_lock<std::shared_mutex> lock(catalogLock_);
        const auto it = tables_.find(name);
        if (it == tables_.end()) {
            if (err != nullptr) {
                *err = "No such table: " + name;
            }
            return false;
        }
        tbl = std::move(it->second);
        tables_.erase(it);
    }

    resetAppendState(*tbl);
    closePrimaryKeyIndex(*tbl);

    std::error_code ec;
    fs::remove(tbl->rowFilePath, ec);
    if (ec && err != nullptr) {
        *err = "Failed to remove table row file: " + tbl->rowFilePath;
        return false;
    }

    if (tbl->primaryKeyIdx >= 0) {
        ec.clear();
        fs::remove(tbl->primaryKeyIndexPath, ec);
        if (ec && err != nullptr) {
            *err = "Failed to remove primary key index: " + tbl->primaryKeyIndexPath;
            return false;
        }
    }
    return true;
}

bool StorageEngine::tableExists(const std::string &name) const {
    std::shared_lock<std::shared_mutex> lock(catalogLock_);
    return tables_.count(name) != 0;
}

Table *StorageEngine::getTable(const std::string &name) {
    std::shared_lock<std::shared_mutex> lock(catalogLock_);
    const auto it = tables_.find(name);
    return (it == tables_.end()) ? nullptr : it->second.get();
}

const Table *StorageEngine::getTable(const std::string &name) const {
    std::shared_lock<std::shared_mutex> lock(catalogLock_);
    const auto it = tables_.find(name);
    return (it == tables_.end()) ? nullptr : it->second.get();
}

std::vector<std::string> StorageEngine::listTables() const {
    std::shared_lock<std::shared_mutex> lock(catalogLock_);
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto &entry : tables_) {
        names.push_back(entry.first);
    }
    return names;
}

bool StorageEngine::getRowFileEndOffset(Table &tbl,
                                        uint64_t &offset,
                                        std::string *err) {
    if (!ensureAppendHandle(tbl, err)) {
        return false;
    }
    offset = tbl.rowFileEndOffset;
    return true;
}

bool StorageEngine::insertRow(Table &tbl,
                              Row row,
                              uint64_t *offset,
                              std::string *err) {
    if (row.values.size() != tbl.schema.size()) {
        return false;
    }

    if (!ensureAppendHandle(tbl, err)) {
        return false;
    }

    std::string payload;
    encodeRowPayload(row, payload);
    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    const uint64_t currentOffset = tbl.rowFileEndOffset;

    const bool ok = writeAll(tbl.rowFileFd, reinterpret_cast<const char *>(&kRowRecordMagic),
                             sizeof(kRowRecordMagic)) &&
                    writeAll(tbl.rowFileFd, reinterpret_cast<const char *>(&payloadSize),
                             sizeof(payloadSize)) &&
                    (payload.empty() || writeAll(tbl.rowFileFd, payload.data(), payload.size()));

    if (!ok) {
        resetAppendState(tbl);
        if (err != nullptr) {
            *err = "Failed to append row to table file";
        }
        return false;
    }

    tbl.rowFileEndOffset += sizeof(kRowRecordMagic) + sizeof(payloadSize) + payloadSize;
    ++tbl.rowCount;
    if (offset != nullptr) {
        *offset = currentOffset;
    }
    return true;
}

bool StorageEngine::insertRowsBatch(Table &tbl,
                                    const std::vector<std::vector<std::string>> &rows,
                                    int64_t expiresAt,
                                    std::vector<uint64_t> *offsets,
                                    std::string *err) {
    if (rows.empty()) {
        return true;
    }

    if (!ensureAppendHandle(tbl, err)) {
        return false;
    }

    /* Close the read fd so it can be reopened with the new data later. */
    closeReadFile(tbl);

    /* Build a single buffer containing all row records. */
    std::string buffer;
    buffer.reserve(rows.size() * (sizeof(kRowRecordMagic) + sizeof(uint32_t) + 64));

    if (offsets != nullptr) {
        offsets->clear();
        offsets->reserve(rows.size());
    }

    uint64_t currentOffset = tbl.rowFileEndOffset;

    for (const std::vector<std::string> &values : rows) {
        if (values.size() != tbl.schema.size()) {
            if (err != nullptr) {
                *err = "Column count mismatch in batch insert";
            }
            return false;
        }

        if (offsets != nullptr) {
            offsets->push_back(currentOffset);
        }

        Row row;
        row.values = values;
        row.expires_at = expiresAt;

        std::string payload;
        encodeRowPayload(row, payload);
        const uint32_t payloadSize = static_cast<uint32_t>(payload.size());

        buffer.append(reinterpret_cast<const char *>(&kRowRecordMagic), sizeof(kRowRecordMagic));
        buffer.append(reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize));
        buffer.append(payload);

        currentOffset += sizeof(kRowRecordMagic) + sizeof(payloadSize) + payloadSize;
    }

    if (!writeAll(tbl.rowFileFd, buffer.data(), buffer.size())) {
        resetAppendState(tbl);
        if (err != nullptr) {
            *err = "Failed to write batch to table file";
        }
        return false;
    }

    tbl.rowFileEndOffset = currentOffset;
    tbl.rowCount += rows.size();
    return true;
}

bool StorageEngine::readRow(const Table &tbl,
                            uint64_t offset,
                            Row &row,
                            std::string *err) const {
    Table &mutableTbl = const_cast<Table &>(tbl);
    if (!ensureReadHandle(mutableTbl, err)) {
        return false;
    }

    bool eof = false;
    return readRowRecord(mutableTbl.readFileFd, offset, row, eof, nullptr, err) && !eof;
}

bool StorageEngine::scanRows(const Table &tbl,
                             const RowVisitor &visitor,
                             std::string *err) const {
    Table &mutableTbl = const_cast<Table &>(tbl);
    if (!ensureReadHandle(mutableTbl, err)) {
        return false;
    }

    uint64_t offset = 0;
    while (true) {
        Row row;
        bool eof = false;
        uint64_t nextOffset = offset;
        if (!readRowRecord(mutableTbl.readFileFd, offset, row, eof, &nextOffset, err)) {
            return false;
        }
        if (eof) {
            break;
        }

        const uint64_t recordOffset = offset;
        offset = nextOffset;

        if (visitor && !visitor(row, recordOffset)) {
            break;
        }
    }

    return true;
}

bool StorageEngine::clearTable(Table &tbl, std::string *err) {
    resetAppendState(tbl);

    int fd = -1;
    if (!openRowFile(tbl.rowFilePath, O_CREAT | O_WRONLY | O_TRUNC, fd, err)) {
        return false;
    }
    closeRowFile(fd);
    tbl.rowCount = 0;
    if (tbl.primaryKeyIdx >= 0 && !resetPrimaryKeyIndex(tbl, err)) {
        return false;
    }
    return true;
}

bool StorageEngine::truncateTable(Table &tbl,
                                  uint64_t endOffset,
                                  uint64_t rowCount,
                                  std::string *err) {
    resetAppendState(tbl);

    int fd = -1;
    if (!openRowFile(tbl.rowFilePath, O_CREAT | O_WRONLY, fd, err)) {
        return false;
    }

    bool ok = (::ftruncate(fd, static_cast<off_t>(endOffset)) == 0);
    if (ok) {
        ok = (::fdatasync(fd) == 0);
    }
    closeRowFile(fd);

    if (!ok) {
        if (err != nullptr && err->empty()) {
            *err = "Failed to truncate table file";
        }
        return false;
    }

    tbl.rowCount = rowCount;
    tbl.rowFileEndOffset = endOffset;
    if (tbl.primaryKeyIdx >= 0 && !rebuildPrimaryKeyIndex(tbl, err)) {
        return false;
    }
    return true;
}

bool StorageEngine::expireRows(Table &tbl, std::string *err) {
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const std::string tempPath = tbl.rowFilePath + ".tmp";
    resetAppendState(tbl);

    int tempFd = -1;
    if (!openRowFile(tempPath, O_CREAT | O_WRONLY | O_TRUNC, tempFd, err)) {
        return false;
    }

    uint64_t kept = 0;
    bool ok = scanRows(tbl, [&](const Row &row, uint64_t) {
        if (row.expires_at != 0 && row.expires_at <= nowMs) {
            return true;
        }

        std::string payload;
        encodeRowPayload(row, payload);
        const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
        const bool writeOk =
            writeAll(tempFd, reinterpret_cast<const char *>(&kRowRecordMagic),
                     sizeof(kRowRecordMagic)) &&
            writeAll(tempFd, reinterpret_cast<const char *>(&payloadSize),
                     sizeof(payloadSize)) &&
            (payload.empty() || writeAll(tempFd, payload.data(), payload.size()));
        if (writeOk) {
            ++kept;
        }
        return writeOk;
    }, err);

    if (ok && ::fdatasync(tempFd) != 0) {
        ok = false;
        if (err != nullptr) {
            *err = "Failed to flush compacted table file";
        }
    }
    closeRowFile(tempFd);

    if (!ok) {
        std::remove(tempPath.c_str());
        return false;
    }

    if (std::rename(tempPath.c_str(), tbl.rowFilePath.c_str()) != 0) {
        if (err != nullptr) {
            *err = "Failed to replace compacted table file";
        }
        std::remove(tempPath.c_str());
        return false;
    }

    tbl.rowCount = kept;
    if (tbl.primaryKeyIdx >= 0) {
        if (!resetPrimaryKeyIndex(tbl, err)) {
            return false;
        }
        std::string rebuildErr;
        const bool rebuildOk = scanRows(tbl, [&](const Row &row, uint64_t offset) {
            return upsertPrimaryKey(tbl, row.values[tbl.primaryKeyIdx], offset, &rebuildErr);
        }, &rebuildErr);
        if (!rebuildOk) {
            if (err != nullptr) {
                *err = rebuildErr.empty() ? "Failed to rebuild primary key index" : rebuildErr;
            }
            return false;
        }
    }
    return true;
}

bool StorageEngine::rebuildPrimaryKeyIndex(Table &tbl, std::string *err) {
    if (tbl.primaryKeyIdx < 0) {
        return true;
    }
    if (!resetPrimaryKeyIndex(tbl, err)) {
        return false;
    }

    std::string rebuildErr;
    const bool rebuildOk = scanRows(tbl, [&](const Row &row, uint64_t offset) {
        return upsertPrimaryKey(tbl, row.values[tbl.primaryKeyIdx], offset, &rebuildErr);
    }, &rebuildErr);
    if (!rebuildOk) {
        if (err != nullptr) {
            *err = rebuildErr.empty() ? "Failed to rebuild primary key index" : rebuildErr;
        }
        return false;
    }
    return true;
}

bool StorageEngine::restoreTableFromBackup(Table &tbl,
                                           const std::string &backupPath,
                                           uint64_t rowCount,
                                           std::string *err) {
    resetAppendState(tbl);
    if (std::rename(backupPath.c_str(), tbl.rowFilePath.c_str()) != 0) {
        if (err != nullptr) {
            *err = "Failed to restore table backup";
        }
        return false;
    }

    tbl.rowCount = rowCount;
    tbl.rowFileEndOffset = 0;
    if (tbl.primaryKeyIdx >= 0 && !rebuildPrimaryKeyIndex(tbl, err)) {
        return false;
    }
    return true;
}

bool StorageEngine::lookupPrimaryKey(Table &tbl,
                                     const std::string &key,
                                     uint64_t &rowOffset,
                                     Row *row,
                                     std::string *err) {
    rowOffset = UINT64_MAX;
    if (tbl.primaryKeyIdx < 0 || !tbl.primaryKeyIndexAvailable) {
        return false;
    }
    if (!ensurePrimaryKeyIndexHandle(tbl, err)) {
        return false;
    }

    const uint64_t fingerprint = primaryKeyFingerprint(key);
    uint64_t slotIndex = fingerprint % tbl.primaryKeyIndexCapacity;
    for (uint64_t probe = 0; probe < tbl.primaryKeyIndexCapacity; ++probe) {
        PrimaryKeyIndexSlot slot;
        if (!readPrimaryKeyIndexSlot(tbl, slotIndex, slot, err)) {
            tbl.primaryKeyIndexAvailable = false;
            return false;
        }
        if (slot.fingerprint == 0) {
            return true;
        }
        if (slot.fingerprint == fingerprint) {
            Row candidate;
            std::string readErr;
            if (!readRow(tbl, slot.rowOffset, candidate, &readErr)) {
                tbl.primaryKeyIndexAvailable = false;
                if (err != nullptr) {
                    *err = readErr.empty() ? "Failed to validate primary key index row" : readErr;
                }
                return false;
            }
            if (static_cast<size_t>(tbl.primaryKeyIdx) < candidate.values.size() &&
                candidate.values[tbl.primaryKeyIdx] == key) {
                rowOffset = slot.rowOffset;
                if (row != nullptr) {
                    *row = std::move(candidate);
                }
                return true;
            }
        }
        slotIndex = (slotIndex + 1) % tbl.primaryKeyIndexCapacity;
    }

    tbl.primaryKeyIndexAvailable = false;
    if (err != nullptr) {
        *err = "Primary key index probe overflow";
    }
    return false;
}

bool StorageEngine::upsertPrimaryKey(Table &tbl,
                                     const std::string &key,
                                     uint64_t rowOffset,
                                     std::string *err) {
    if (tbl.primaryKeyIdx < 0 || !tbl.primaryKeyIndexAvailable) {
        return false;
    }
    if (!ensurePrimaryKeyIndexHandle(tbl, err) || !maybeGrowPrimaryKeyIndex(tbl, err)) {
        tbl.primaryKeyIndexAvailable = false;
        return false;
    }

    const uint64_t fingerprint = primaryKeyFingerprint(key);
    uint64_t slotIndex = fingerprint % tbl.primaryKeyIndexCapacity;
    for (uint64_t probe = 0; probe < tbl.primaryKeyIndexCapacity; ++probe) {
        PrimaryKeyIndexSlot slot;
        if (!readPrimaryKeyIndexSlot(tbl, slotIndex, slot, err)) {
            tbl.primaryKeyIndexAvailable = false;
            return false;
        }

        if (slot.fingerprint == 0) {
            const PrimaryKeyIndexSlot inserted{fingerprint, rowOffset};
            if (!writePrimaryKeyIndexSlot(tbl, slotIndex, inserted, err)) {
                tbl.primaryKeyIndexAvailable = false;
                return false;
            }
            ++tbl.primaryKeyIndexEntries;
            if (!writePrimaryKeyIndexHeader(tbl, err)) {
                tbl.primaryKeyIndexAvailable = false;
                return false;
            }
            return true;
        }

        if (slot.fingerprint == fingerprint) {
            Row candidate;
            std::string readErr;
            if (!readRow(tbl, slot.rowOffset, candidate, &readErr)) {
                tbl.primaryKeyIndexAvailable = false;
                if (err != nullptr) {
                    *err = readErr.empty() ? "Failed to validate primary key index row" : readErr;
                }
                return false;
            }
            if (static_cast<size_t>(tbl.primaryKeyIdx) < candidate.values.size() &&
                candidate.values[tbl.primaryKeyIdx] == key) {
                slot.rowOffset = rowOffset;
                if (!writePrimaryKeyIndexSlot(tbl, slotIndex, slot, err)) {
                    tbl.primaryKeyIndexAvailable = false;
                    return false;
                }
                return true;
            }
        }

        slotIndex = (slotIndex + 1) % tbl.primaryKeyIndexCapacity;
    }

    tbl.primaryKeyIndexAvailable = false;
    if (err != nullptr) {
        *err = "Primary key index insertion overflow";
    }
    return false;
}

bool StorageEngine::hasPrimaryKeyIndex(const Table &tbl) const {
    return tbl.primaryKeyIdx >= 0 && tbl.primaryKeyIndexAvailable;
}

uint64_t StorageEngine::rowCount(const Table &tbl) const {
    return tbl.rowCount;
}

int StorageEngine::colIndex(const Table &tbl, const std::string &colName) const {
    for (size_t i = 0; i < tbl.schema.size(); ++i) {
        if (tbl.schema[i].name == colName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
