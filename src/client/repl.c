/*
 * FlexQL: Interactive REPL Client
 * Usage: ./flexql-client <host> <port>
 *
 */

#include "flexql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Callback: prints each row as KEY = VALUE per line */
static int printCallback(void *data, int ncols, char **vals, char **names) {
    (void)data;
    for (int i = 0; i < ncols; i++) {
        printf("%s = %s\n", names[i] ? names[i] : "?",
                             vals[i]  ? vals[i]  : "NULL");
    }
    printf("\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);

    FlexQL *db = NULL;
    int rc = flexql_open(host, port, &db);
    if (rc != FLEXQL_OK) {
        fprintf(stderr, "Cannot connect to FlexQL server at %s:%d\n", host, port);
        return 1;
    }
    printf("Connected to FlexQL server\n");

    char lineBuf[65536];
    char multiLine[1048576];
    multiLine[0] = '\0';

    while (1) {
        printf("flexql> ");
        fflush(stdout);

        if (!fgets(lineBuf, sizeof(lineBuf), stdin)) break;

        /* Strip newline */
        size_t len = strlen(lineBuf);
        if (len > 0 && lineBuf[len-1] == '\n') lineBuf[--len] = '\0';

        if (strcmp(lineBuf, ".exit") == 0 || strcmp(lineBuf, ".quit") == 0) {
            printf("Connection closed\n");
            break;
        }
        if (strcmp(lineBuf, ".help") == 0) {
            printf("Commands:\n"
                   "  CREATE TABLE name (col TYPE, ...);\n"
                   "  INSERT INTO name VALUES (v1, v2, ...);\n"
                   "  SELECT * FROM name [WHERE col = val];\n"
                   "  SELECT col1,col2 FROM name;\n"
                   "  SELECT * FROM t1 INNER JOIN t2 ON t1.c = t2.c;\n"
                   "  .exit / .quit   - Disconnect\n"
                   "  .help           - This message\n");
            continue;
        }

        /* Accumulate multi-line input until ';' */
        strcat(multiLine, lineBuf);
        strcat(multiLine, " ");

        /* Check if statement is complete (ends with ;) */
        char *semi = strrchr(multiLine, ';');
        if (!semi) continue;  /* Wait for more input */

        /* Execute */
        char *errmsg = NULL;
        rc = flexql_exec(db, multiLine, printCallback, NULL, &errmsg);
        if (rc != FLEXQL_OK) {
            fprintf(stderr, "Error: %s\n", errmsg ? errmsg : "unknown error");
            flexql_free(errmsg);
        } else {
            /* Only print "OK" for non-SELECT (SELECT already printed rows) */
        }

        multiLine[0] = '\0';
    }

    flexql_close(db);
    return 0;
}
