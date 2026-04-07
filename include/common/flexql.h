/*
 * FlexQL: A Flexible SQL-like Database Driver
 * Main public API header
 *
 */

#ifndef FLEXQL_H
#define FLEXQL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Error Codes ─────────────────────────────────────────────────────────── */
#define FLEXQL_OK            0
#define FLEXQL_ERROR         1
#define FLEXQL_NOMEM         2
#define FLEXQL_BUSY          3
#define FLEXQL_TIMEOUT       4
#define FLEXQL_NOTFOUND      5

/* ─── Opaque Database Handle ──────────────────────────────────────────────── */
typedef struct FlexQL FlexQL;

/* ─── Callback Prototype ──────────────────────────────────────────────────── */
/*
 * callback(data, columnCount, values, columnNames)
 *   Returns 0 to continue, 1 to abort query
 */
typedef int (*flexql_callback)(void *data, int columnCount,
                               char **values, char **columnNames);

/* ─── Public API ──────────────────────────────────────────────────────────── */

/*
 * flexql_open: Connect to a FlexQL server.
 * Returns FLEXQL_OK on success, FLEXQL_ERROR on failure.
 */
int flexql_open(const char *host, int port, FlexQL **db);

/*
 * flexql_close: Close connection and free resources.
 */
int flexql_close(FlexQL *db);

/*
 * flexql_exec: Execute an SQL statement.
 * Invokes callback once per result row (if non-NULL).
 */
int flexql_exec(FlexQL *db,
                const char *sql,
                flexql_callback callback,
                void *arg,
                char **errmsg);

/*
 * flexql_free: Free memory allocated by FlexQL API.
 */
void flexql_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* FLEXQL_H */
