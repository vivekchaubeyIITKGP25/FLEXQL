/*
 * FlexQL: Server-side client session handling.
 */

#ifndef FLEXQL_SERVER_SESSION_H
#define FLEXQL_SERVER_SESSION_H

class Executor;

void serveClientConnection(int clientFd, Executor &executor);

#endif /* FLEXQL_SERVER_SESSION_H */
