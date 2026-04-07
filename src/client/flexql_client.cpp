/*
 * FlexQL: Client Library Implementation
 * Implements the public C API: flexql_open, flexql_close, flexql_exec, flexql_free
 *
 */

#include "flexql.h"
#include "network.h"
#include "protocol.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

/* ─── Internal handle ─────────────────────────────────────────────────────── */

struct FlexQL {
    int sockfd = -1;
};

int flexql_open_fd_for_test(int fd, FlexQL **db) {
    if (fd < 0 || !db) return FLEXQL_ERROR;

    FlexQL *handle = new FlexQL();
    handle->sockfd = fd;
    *db = handle;
    return FLEXQL_OK;
}

/* ─── flexql_open ─────────────────────────────────────────────────────────── */

int flexql_open(const char *host, int port, FlexQL **db) {
    if (!host || !db) return FLEXQL_ERROR;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return FLEXQL_ERROR;

    /* Resolve host */
    struct hostent *he = gethostbyname(host);
    if (!he) { ::close(fd); return FLEXQL_ERROR; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return FLEXQL_ERROR;
    }

    net::setNoDelay(fd);
    net::setBufferSize(fd, 256 * 1024);

    FlexQL *handle = new FlexQL();
    handle->sockfd = fd;
    *db = handle;
    return FLEXQL_OK;
}

/* ─── flexql_close ────────────────────────────────────────────────────────── */

int flexql_close(FlexQL *db) {
    if (!db) return FLEXQL_ERROR;
    if (db->sockfd >= 0) {
        net::sendMsg(db->sockfd, protocol::kExit);
        ::close(db->sockfd);
        db->sockfd = -1;
    }
    delete db;
    return FLEXQL_OK;
}

/* ─── flexql_exec ─────────────────────────────────────────────────────────── */

int flexql_exec(FlexQL *db,
                const char *sql,
                flexql_callback callback,
                void *arg,
                char **errmsg) {
    if (errmsg) *errmsg = nullptr;

    if (!db || db->sockfd < 0 || !sql) {
        if (errmsg) *errmsg = strdup("Invalid database handle");
        return FLEXQL_ERROR;
    }

    /* Send query */
    if (!net::sendMsg(db->sockfd, std::string(protocol::kQueryPrefix) + sql)) {
        if (errmsg) *errmsg = strdup("Failed to send query");
        return FLEXQL_ERROR;
    }

    std::vector<std::string> colNames;
    bool abortRequested = false;

    while (true) {
        std::string frame;
        if (!net::recvMsg(db->sockfd, frame)) {
            if (errmsg) *errmsg = strdup("Failed to receive response");
            return FLEXQL_ERROR;
        }

        if (frame == protocol::kEnd || frame == protocol::kAborted) {
            return FLEXQL_OK;
        }

        if (frame.rfind(protocol::kErrorPrefix, 0) == 0) {
            std::string msg = frame.substr(std::string(protocol::kErrorPrefix).size());
            if (errmsg) *errmsg = strdup(msg.c_str());
            return FLEXQL_ERROR;
        }

        if (frame.rfind(protocol::kHeaderPrefix, 0) == 0) {
            colNames.clear();
            std::istringstream hs(frame.substr(std::string(protocol::kHeaderPrefix).size()));
            std::string col;
            while (std::getline(hs, col, '\t')) {
                colNames.push_back(col);
            }
            continue;
        }

        if (frame.rfind(protocol::kRowPrefix, 0) != 0) {
            continue;
        }
        if (!callback || abortRequested) {
            continue;
        }

        std::vector<std::string> vals;
        std::istringstream rs(frame.substr(std::string(protocol::kRowPrefix).size()));
        std::string v;
        while (std::getline(rs, v, '\t')) {
            vals.push_back(v);
        }

        while (vals.size() < colNames.size()) vals.emplace_back("");

        std::vector<char *> cvals(vals.size()), cnames(colNames.size());
        for (size_t i = 0; i < vals.size(); ++i) {
            cvals[i] = const_cast<char *>(vals[i].c_str());
        }
        for (size_t i = 0; i < colNames.size(); ++i) {
            cnames[i] = const_cast<char *>(colNames[i].c_str());
        }

        if (callback(arg, static_cast<int>(colNames.size()), cvals.data(), cnames.data()) != 0) {
            abortRequested = true;
            if (!net::sendMsg(db->sockfd, protocol::kAbort)) {
                if (errmsg) *errmsg = strdup("Failed to abort query");
                return FLEXQL_ERROR;
            }
        }
    }
}

/* ─── flexql_free ─────────────────────────────────────────────────────────── */

void flexql_free(void *ptr) {
    free(ptr);
}
