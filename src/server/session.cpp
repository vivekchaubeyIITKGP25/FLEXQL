/*
 * FlexQL: Server-side client session handling.
 */

#include "session.h"

#include "executor.h"
#include "network.h"
#include "parser.h"
#include "protocol.h"

#include <string>
#include <vector>

#include <unistd.h>

namespace {

bool startsWith(const std::string &value, const char *prefix) {
    const std::string needle(prefix);
    return value.size() >= needle.size() &&
           value.compare(0, needle.size(), needle) == 0;
}

std::string joinColumns(int ncols, char **values) {
    std::string payload;
    for (int i = 0; i < ncols; ++i) {
        payload += (values[i] != nullptr) ? values[i] : "";
        if (i + 1 < ncols) {
            payload += '\t';
        }
    }
    return payload;
}

bool sendErrorFrame(int fd, const std::string &message) {
    return net::sendMsg(fd, std::string(protocol::kErrorPrefix) + message);
}

} // namespace

void serveClientConnection(int clientFd, Executor &executor) {
    Parser parser;
    std::string request;

    while (net::recvMsg(clientFd, request)) {
        if (request == protocol::kExit) {
            break;
        }
        if (request == protocol::kAbort) {
            continue;
        }
        if (!startsWith(request, protocol::kQueryPrefix)) {
            if (!sendErrorFrame(clientFd, "Invalid request frame")) {
                break;
            }
            continue;
        }

        const std::string sql = request.substr(std::string(protocol::kQueryPrefix).size());

        ParsedQuery query;
        try {
            query = parser.parse(sql);
        } catch (const std::exception &ex) {
            if (!sendErrorFrame(clientFd, ex.what())) {
                break;
            }
            continue;
        }

        if (!query.error.empty()) {
            if (!sendErrorFrame(clientFd, query.error)) {
                break;
            }
            continue;
        }

        struct StreamCtx {
            int fd = -1;
            bool headerSent = false;
            bool clientAborted = false;
            bool streamFailed = false;
        } ctx{clientFd};

        auto callback = [](void *data, int ncols, char **vals, char **names) -> int {
            StreamCtx *ctx = static_cast<StreamCtx *>(data);
            if (!ctx->headerSent) {
                if (!net::sendMsg(ctx->fd,
                                  std::string(protocol::kHeaderPrefix) +
                                  joinColumns(ncols, names))) {
                    ctx->streamFailed = true;
                    return 1;
                }
                ctx->headerSent = true;
            }

            if (!net::sendMsg(ctx->fd,
                              std::string(protocol::kRowPrefix) +
                              joinColumns(ncols, vals))) {
                ctx->streamFailed = true;
                return 1;
            }

            std::string control;
            bool hasControl = false;
            if (!net::tryRecvMsg(ctx->fd, control, hasControl)) {
                ctx->streamFailed = true;
                return 1;
            }
            if (hasControl && control == protocol::kAbort) {
                ctx->clientAborted = true;
                return 1;
            }
            return 0;
        };

        std::string errMsg;
        const int rc = executor.execute(query, callback, &ctx, errMsg);
        if (ctx.streamFailed) {
            break;
        }
        if (rc != FLEXQL_OK) {
            if (!sendErrorFrame(clientFd, errMsg)) {
                break;
            }
            continue;
        }

        if (!net::sendMsg(clientFd,
                          ctx.clientAborted ? protocol::kAborted : protocol::kEnd)) {
            break;
        }
    }

    ::close(clientFd);
}
