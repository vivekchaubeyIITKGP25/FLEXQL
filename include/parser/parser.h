/*
 * FlexQL: SQL parser for the supported SQL subset.
 */

#ifndef FLEXQL_PARSER_H
#define FLEXQL_PARSER_H

#include "types.h"

#include <string>
#include <vector>

enum class QueryType {
    CREATE_TABLE,
    INSERT,
    SELECT,
    DELETE_ROWS,
    UNKNOWN
};

struct ParsedQuery {
    QueryType   type = QueryType::UNKNOWN;
    std::string tableName;

    /* CREATE TABLE */
    std::vector<ColumnDef> columns;
    bool                   createIfNotExists = false;

    /* INSERT */
    std::vector<std::string>              insertValues;
    std::vector<std::vector<std::string>> insertRows;
    std::vector<std::vector<std::string>> insertRawRows;
    int64_t                               expiresAt = 0;

    /* SELECT */
    std::vector<std::string> selectCols;
    WhereClause              where;
    JoinClause               join;
    OrderByClause            orderBy;

    /* Error */
    std::string error;
};

class Parser {
public:
    ParsedQuery parse(const std::string &sql);

private:
    ParsedQuery parseCreate(const std::string &sql);
    ParsedQuery parseInsert(const std::string &sql);
    ParsedQuery parseSelect(const std::string &sql);
    ParsedQuery parseDelete(const std::string &sql);

    static std::string trim(const std::string &s);
    static std::string toUpper(const std::string &s);
    static std::vector<std::string> splitComma(const std::string &s);
    static std::string stripQuotes(const std::string &s);
};

#endif /* FLEXQL_PARSER_H */
