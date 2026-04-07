/*
 * FlexQL: Durable append-only log for crash recovery.
 */

#ifndef FLEXQL_DURABLE_LOG_H
#define FLEXQL_DURABLE_LOG_H

#include <cstdint>
#include <mutex>
#include <string>

struct ParsedQuery;
class StorageEngine;

class DurableLog {
public:
    explicit DurableLog(std::string path);
    ~DurableLog();

    DurableLog(const DurableLog &) = delete;
    DurableLog &operator=(const DurableLog &) = delete;

    bool open(std::string &err);
    void close();

    bool beginCreate(const ParsedQuery &query, uint64_t &txId, std::string &err);
    bool beginInsert(const ParsedQuery &query, uint64_t &txId, std::string &err);
    bool beginInsertNoSync(const ParsedQuery &query, uint64_t &txId, std::string &err);
    bool beginDelete(const ParsedQuery &query, uint64_t &txId, std::string &err);
    bool commit(uint64_t txId, std::string &err);
    bool commitNoSync(uint64_t txId, std::string &err);
    bool sync(std::string &err);

    const std::string &path() const;

private:
    bool appendRecord(uint32_t type,
                      uint64_t &txId,
                      const std::string &payload,
                      bool includeTxIdInPayload,
                      bool doSync,
                      std::string &err);

    std::string path_;
    int fd_ = -1;
    std::mutex mutex_;
};

bool replayDurableLog(const std::string &path,
                      StorageEngine &storage,
                      std::string &err);

#endif /* FLEXQL_DURABLE_LOG_H */
