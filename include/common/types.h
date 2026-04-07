/*
 * FlexQL: Common types and definitions.
 */

#ifndef FLEXQL_TYPES_H
#define FLEXQL_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

enum class ColType {
    INT,
    DECIMAL,
    VARCHAR,
    DATETIME
};

inline std::string colTypeToStr(ColType t) {
    switch (t) {
        case ColType::INT:      return "INT";
        case ColType::DECIMAL:  return "DECIMAL";
        case ColType::VARCHAR:  return "VARCHAR";
        case ColType::DATETIME: return "DATETIME";
    }
    return "UNKNOWN";
}

inline ColType strToColType(const std::string &s) {
    if (s == "INT")      return ColType::INT;
    if (s == "DECIMAL")  return ColType::DECIMAL;
    if (s == "VARCHAR")  return ColType::VARCHAR;
    if (s == "TEXT")     return ColType::VARCHAR;
    if (s == "DATETIME") return ColType::DATETIME;
    return ColType::VARCHAR;
}

struct ColumnDef {
    std::string name;
    ColType     type;
    bool        notNull = false;
    bool        primaryKey = false;
};

struct Row {
    std::vector<std::string> values;
    int64_t                  expires_at = 0;
};

struct WhereClause {
    bool        active = false;
    std::string column;
    std::string op;
    std::string value;
};

struct OrderByClause {
    bool        active = false;
    std::string column;
    bool        ascending = true;
};

struct JoinClause {
    bool        active = false;
    std::string rightTable;
    std::string leftCol;
    std::string op = "=";
    std::string rightCol;
};

#define FLEXQL_MSG_DELIM '\x1E'
#define FLEXQL_MAX_MSG   (16 * 1024 * 1024)

#endif /* FLEXQL_TYPES_H */
