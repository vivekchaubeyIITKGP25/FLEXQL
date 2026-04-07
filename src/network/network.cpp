/*
 * FlexQL: Network Implementation
 * Length-prefixed framing: [4-byte BE uint32 length][payload]
 *
 */

#include "network.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/ioctl.h>

namespace net {

static bool writeAll(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, buf + sent, len - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool readAll(int fd, char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::read(fd, buf + got, len - got);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool sendMsg(int fd, const std::string &msg) {
    uint32_t len = htonl((uint32_t)msg.size());
    if (!writeAll(fd, (const char *)&len, 4)) return false;
    if (!msg.empty() && !writeAll(fd, msg.data(), msg.size())) return false;
    return true;
}

bool recvMsg(int fd, std::string &msg) {
    uint32_t nlen = 0;
    if (!readAll(fd, (char *)&nlen, 4)) return false;
    uint32_t len = ntohl(nlen);
    if (len > (uint32_t)FLEXQL_MAX_MSG) return false;
    msg.resize(len);
    if (len > 0 && !readAll(fd, &msg[0], len)) return false;
    return true;
}

bool tryRecvMsg(int fd, std::string &msg, bool &hasMsg) {
    hasMsg = false;

    int available = 0;
    if (::ioctl(fd, FIONREAD, &available) != 0) {
        return false;
    }
    if (available < 4) {
        return true;
    }

    uint32_t nlen = 0;
    const ssize_t peeked = ::recv(fd, &nlen, sizeof(nlen), MSG_PEEK | MSG_DONTWAIT);
    if (peeked < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }
    if (peeked == 0) {
        return false;
    }
    if (peeked != static_cast<ssize_t>(sizeof(nlen))) {
        return true;
    }

    const uint32_t len = ntohl(nlen);
    if (len > static_cast<uint32_t>(FLEXQL_MAX_MSG)) {
        return false;
    }
    if (available < static_cast<int>(sizeof(nlen) + len)) {
        return true;
    }

    hasMsg = recvMsg(fd, msg);
    return hasMsg;
}

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void setNoDelay(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

void setBufferSize(int fd, int size) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

} // namespace net
