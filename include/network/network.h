/*
 * FlexQL: Length-prefixed network helpers.
 */

#ifndef FLEXQL_NETWORK_H
#define FLEXQL_NETWORK_H

#include "types.h"

#include <string>

namespace net {

bool sendMsg(int fd, const std::string &msg);
bool recvMsg(int fd, std::string &msg);
bool tryRecvMsg(int fd, std::string &msg, bool &hasMsg);
void setNonBlocking(int fd);
void setNoDelay(int fd);
void setBufferSize(int fd, int size);

} // namespace net

#endif /* FLEXQL_NETWORK_H */
