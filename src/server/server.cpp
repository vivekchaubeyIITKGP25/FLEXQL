/*
 * FlexQL: Multithreaded server.
 */

#include "durable_log.h"
#include "executor.h"
#include "lru_cache.h"
#include "network.h"
#include "session.h"
#include "storage.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

class ThreadPool {
public:
    explicit ThreadPool(size_t workerCount) {
        for (size_t i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread &worker : workers_) {
            worker.join();
        }
    }

    template <typename Fn>
    void submit(Fn &&fn) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.push(std::forward<Fn>(fn));
        }
        cv_.notify_one();
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

static std::atomic<bool> g_running{true};
static int g_serverFd = -1;
static constexpr char kDataPath[] = "data/flexql.wal";

static void signalHandler(int) {
    g_running = false;
    if (g_serverFd >= 0) {
        ::close(g_serverFd);
        g_serverFd = -1;
    }
}

int main(int argc, char *argv[]) {
    int port = 9000;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }
    const char *envDataPath = std::getenv("FLEXQL_DATA_PATH");
    const char *legacySnapshotPath = std::getenv("FLEXQL_SNAPSHOT_PATH");
    const char *envStorageRoot = std::getenv("FLEXQL_STORAGE_ROOT");
    const std::string dataPath =
        (argc >= 3 && argv[2] != nullptr && argv[2][0] != '\0') ? argv[2]
        : (envDataPath != nullptr && envDataPath[0] != '\0') ? envDataPath
        : (legacySnapshotPath != nullptr && legacySnapshotPath[0] != '\0') ? legacySnapshotPath
        : kDataPath;
    const std::string storageRoot =
        (envStorageRoot != nullptr && envStorageRoot[0] != '\0') ? envStorageRoot
        : (dataPath + ".tables");

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    StorageEngine storage(storageRoot);
    LRUCache cache(4096, 128ULL * 1024 * 1024);
    std::string storageErr;
    if (!replayDurableLog(dataPath, storage, storageErr)) {
        std::cerr << "Failed to replay durable log: " << storageErr << "\n";
        return 1;
    }

    DurableLog durableLog(dataPath);
    if (!durableLog.open(storageErr)) {
        std::cerr << "Failed to open durable log: " << storageErr << "\n";
        return 1;
    }

    Executor executor(storage, cache, &durableLog);
    for (const std::string &tableName : storage.listTables()) {
        executor.rebuildIndex(tableName);
    }

    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        perror("socket");
        return 1;
    }
    g_serverFd = serverFd;

    int reuseAddr = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(serverFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        ::close(serverFd);
        g_serverFd = -1;
        return 1;
    }

    if (::listen(serverFd, 128) < 0) {
        perror("listen");
        ::close(serverFd);
        g_serverFd = -1;
        return 1;
    }

    const unsigned int hardware = std::thread::hardware_concurrency();
    const size_t workerCount = std::max<size_t>(4, hardware == 0 ? 4 : hardware * 2);
    {
        ThreadPool pool(workerCount);

        std::cout << "FlexQL server listening on port " << port
                  << " (" << workerCount << " worker threads)\n";

        while (g_running) {
            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            const int clientFd =
                ::accept(serverFd, reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);
            if (clientFd < 0) {
                if (!g_running) {
                    break;
                }
                continue;
            }

            net::setNoDelay(clientFd);
            net::setBufferSize(clientFd, 256 * 1024);

            pool.submit([clientFd, &executor]() {
                serveClientConnection(clientFd, executor);
            });
        }
    }

    if (g_serverFd >= 0) {
        ::close(g_serverFd);
        g_serverFd = -1;
    }
    durableLog.close();

    std::cout << "FlexQL server shut down.\n";
    return 0;
}
