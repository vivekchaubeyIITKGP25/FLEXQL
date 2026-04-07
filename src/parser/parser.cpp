/*
 * FlexQL: SQL parser implementation.
 */

#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace {

std::string trimCopy(const std::string &s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string upperCopy(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

bool isIdentifierChar(char ch) {
    unsigned char uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '_';
}

bool keywordBoundary(const std::string &text, size_t pos, size_t len) {
    const bool leftOk = (pos == 0) || !isIdentifierChar(text[pos - 1]);
    const size_t rightPos = pos + len;
    const bool rightOk = (rightPos >= text.size()) || !isIdentifierChar(text[rightPos]);
    return leftOk && rightOk;
}

size_t findKeywordOutside(const std::string &text,
                          const std::string &keyword,
                          size_t start = 0) {
    const std::string upperText = upperCopy(text);
    const std::string upperKeyword = upperCopy(keyword);
    bool inSingle = false;
    bool inDouble = false;
    int depth = 0;

    for (size_t i = start; i + upperKeyword.size() <= text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (ch == '"' && !inSingle) {
            inDouble = !inDouble;
        } else if (!inSingle && !inDouble) {
            if (ch == '(') {
                ++depth;
            } else if (ch == ')' && depth > 0) {
                --depth;
            }
        }

        if (inSingle || inDouble || depth != 0) {
            continue;
        }
        if (upperText.compare(i, upperKeyword.size(), upperKeyword) == 0 &&
            keywordBoundary(upperText, i, upperKeyword.size())) {
            return i;
        }
    }
    return std::string::npos;
}

bool startsWithKeyword(const std::string &text, const std::string &keyword) {
    const std::string trimmed = trimCopy(text);
    const std::string upperText = upperCopy(trimmed);
    const std::string upperKeyword = upperCopy(keyword);
    return upperText.compare(0, upperKeyword.size(), upperKeyword) == 0 &&
           keywordBoundary(upperText, 0, upperKeyword.size());
}

size_t findMatchingParen(const std::string &text, size_t openPos) {
    bool inSingle = false;
    bool inDouble = false;
    int depth = 0;

    for (size_t i = openPos; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
            continue;
        }
        if (ch == '"' && !inSingle) {
            inDouble = !inDouble;
            continue;
        }
        if (inSingle || inDouble) {
            continue;
        }
        if (ch == '(') {
            ++depth;
        } else if (ch == ')') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string::npos;
}

std::vector<std::string> splitCommaAware(const std::string &s) {
    std::vector<std::string> parts;
    bool inSingle = false;
    bool inDouble = false;
    int depth = 0;
    std::string current;

    for (char ch : s) {
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (ch == '"' && !inSingle) {
            inDouble = !inDouble;
        } else if (!inSingle && !inDouble) {
            if (ch == '(') {
                ++depth;
            } else if (ch == ')' && depth > 0) {
                --depth;
            }
        }

        if (ch == ',' && !inSingle && !inDouble && depth == 0) {
            parts.push_back(trimCopy(current));
            current.clear();
            continue;
        }
        current += ch;
    }

    if (!trimCopy(current).empty()) {
        parts.push_back(trimCopy(current));
    }
    return parts;
}

std::string stripQuotesCopy(const std::string &s) {
    const std::string trimmed = trimCopy(s);
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '\'' && trimmed.back() == '\'') ||
         (trimmed.front() == '"' && trimmed.back() == '"'))) {
        return trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed;
}

bool containsLogicalCombination(const std::string &expr) {
    return findKeywordOutside(expr, "AND") != std::string::npos ||
           findKeywordOutside(expr, "OR") != std::string::npos;
}

bool parseConditionExpr(const std::string &expr,
                        std::string &lhs,
                        std::string &op,
                        std::string &rhs,
                        std::string &err) {
    if (containsLogicalCombination(expr)) {
        err = "Only one WHERE condition is supported";
        return false;
    }

    static const char *kOps[] = {">=", "<=", "!=", "=", ">", "<"};
    bool inSingle = false;
    bool inDouble = false;
    int depth = 0;

    for (size_t i = 0; i < expr.size(); ++i) {
        const char ch = expr[i];
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
            continue;
        }
        if (ch == '"' && !inSingle) {
            inDouble = !inDouble;
            continue;
        }
        if (inSingle || inDouble) {
            continue;
        }
        if (ch == '(') {
            ++depth;
            continue;
        }
        if (ch == ')' && depth > 0) {
            --depth;
            continue;
        }
        if (depth != 0) {
            continue;
        }

        for (const char *candidate : kOps) {
            const size_t opLen = std::strlen(candidate);
            if (i + opLen <= expr.size() && expr.compare(i, opLen, candidate) == 0) {
                lhs = trimCopy(expr.substr(0, i));
                op = candidate;
                rhs = trimCopy(expr.substr(i + opLen));
                if (lhs.empty() || rhs.empty()) {
                    err = "Malformed condition";
                    return false;
                }
                return true;
            }
        }
    }

    err = "Malformed condition";
    return false;
}

std::pair<std::string, std::string> splitQualifiedName(const std::string &ref) {
    const std::string trimmed = trimCopy(ref);
    const size_t dotPos = trimmed.find('.');
    if (dotPos == std::string::npos) {
        return {"", trimmed};
    }
    return {
        trimCopy(trimmed.substr(0, dotPos)),
        trimCopy(trimmed.substr(dotPos + 1))
    };
}

std::string canonicalTypeToken(const std::string &token) {
    const std::string upper = upperCopy(trimCopy(token));
    const size_t parenPos = upper.find('(');
    return (parenPos == std::string::npos) ? upper : upper.substr(0, parenPos);
}

} // namespace

std::string Parser::trim(const std::string &s) {
    return trimCopy(s);
}

std::string Parser::toUpper(const std::string &s) {
    return upperCopy(s);
}

std::vector<std::string> Parser::splitComma(const std::string &s) {
    return splitCommaAware(s);
}

std::string Parser::stripQuotes(const std::string &s) {
    return stripQuotesCopy(s);
}

ParsedQuery Parser::parse(const std::string &rawSql) {
    std::string sql = trimCopy(rawSql);
    if (!sql.empty() && sql.back() == ';') {
        sql.pop_back();
    }
    sql = trimCopy(sql);

    ParsedQuery q;
    if (sql.empty()) {
        q.error = "Empty SQL statement";
        return q;
    }

    if (startsWithKeyword(sql, "CREATE")) {
        return parseCreate(sql);
    }
    if (startsWithKeyword(sql, "INSERT")) {
        return parseInsert(sql);
    }
    if (startsWithKeyword(sql, "SELECT")) {
        return parseSelect(sql);
    }
    if (startsWithKeyword(sql, "DELETE")) {
        return parseDelete(sql);
    }

    q.error = "Unknown SQL command";
    return q;
}

ParsedQuery Parser::parseCreate(const std::string &sql) {
    ParsedQuery q;
    q.type = QueryType::CREATE_TABLE;

    if (!startsWithKeyword(sql, "CREATE TABLE")) {
        q.error = "Invalid CREATE TABLE syntax";
        return q;
    }

    std::string rest = trimCopy(sql.substr(std::string("CREATE TABLE").size()));
    if (startsWithKeyword(rest, "IF NOT EXISTS")) {
        q.createIfNotExists = true;
        rest = trimCopy(rest.substr(std::string("IF NOT EXISTS").size()));
    }

    const size_t openPos = rest.find('(');
    if (openPos == std::string::npos) {
        q.error = "Invalid CREATE TABLE syntax";
        return q;
    }

    q.tableName = trimCopy(rest.substr(0, openPos));
    if (q.tableName.empty()) {
        q.error = "Missing table name";
        return q;
    }

    const size_t closePos = findMatchingParen(rest, openPos);
    if (closePos == std::string::npos || !trimCopy(rest.substr(closePos + 1)).empty()) {
        q.error = "Invalid CREATE TABLE syntax";
        return q;
    }

    const auto definitions = splitCommaAware(rest.substr(openPos + 1, closePos - openPos - 1));
    if (definitions.empty()) {
        q.error = "CREATE TABLE requires at least one column";
        return q;
    }

    for (const std::string &definition : definitions) {
        std::istringstream ss(definition);
        std::vector<std::string> words;
        std::string word;
        while (ss >> word) {
            words.push_back(word);
        }
        if (words.size() < 2) {
            q.error = "Bad column definition: " + definition;
            return q;
        }

        ColumnDef col;
        col.name = trimCopy(words[0]);
        const std::string typeToken = canonicalTypeToken(words[1]);
        if (typeToken != "INT" &&
            typeToken != "DECIMAL" &&
            typeToken != "VARCHAR" &&
            typeToken != "TEXT" &&
            typeToken != "DATETIME") {
            q.error = "Unsupported column type: " + words[1];
            return q;
        }
        col.type = strToColType(typeToken);

        const std::string upperDef = upperCopy(definition);
        col.notNull = upperDef.find("NOT NULL") != std::string::npos;
        col.primaryKey = upperDef.find("PRIMARY KEY") != std::string::npos;
        q.columns.push_back(col);
    }

    return q;
}

ParsedQuery Parser::parseInsert(const std::string &sql) {
    ParsedQuery q;
    q.type = QueryType::INSERT;

    if (!startsWithKeyword(sql, "INSERT INTO")) {
        q.error = "Invalid INSERT syntax";
        return q;
    }

    const size_t valuesPos = findKeywordOutside(sql, "VALUES");
    if (valuesPos == std::string::npos) {
        q.error = "Invalid INSERT syntax";
        return q;
    }

    q.tableName = trimCopy(sql.substr(std::string("INSERT INTO").size(),
                                      valuesPos - std::string("INSERT INTO").size()));
    if (q.tableName.empty()) {
        q.error = "Missing table name";
        return q;
    }

    std::string valuesExpr = trimCopy(sql.substr(valuesPos + std::string("VALUES").size()));
    const size_t expiresPos = findKeywordOutside(valuesExpr, "EXPIRES");
    if (expiresPos != std::string::npos) {
        const std::string expiresText =
            trimCopy(valuesExpr.substr(expiresPos + std::string("EXPIRES").size()));
        valuesExpr = trimCopy(valuesExpr.substr(0, expiresPos));
        if (expiresText.empty()) {
            q.error = "EXPIRES requires a unix timestamp";
            return q;
        }
        try {
            q.expiresAt = std::stoll(expiresText);
        } catch (...) {
            q.error = "Invalid EXPIRES timestamp";
            return q;
        }
    }

    size_t pos = 0;
    while (pos < valuesExpr.size()) {
        while (pos < valuesExpr.size() &&
               (std::isspace(static_cast<unsigned char>(valuesExpr[pos])) || valuesExpr[pos] == ',')) {
            ++pos;
        }
        if (pos >= valuesExpr.size()) {
            break;
        }
        if (valuesExpr[pos] != '(') {
            q.error = "Invalid INSERT VALUES syntax";
            return q;
        }

        const size_t closePos = findMatchingParen(valuesExpr, pos);
        if (closePos == std::string::npos) {
            q.error = "Invalid INSERT VALUES syntax";
            return q;
        }

        const auto rawValues = splitCommaAware(valuesExpr.substr(pos + 1, closePos - pos - 1));
        if (rawValues.empty()) {
            q.error = "INSERT requires at least one value";
            return q;
        }

        std::vector<std::string> parsedValues;
        parsedValues.reserve(rawValues.size());
        for (const std::string &rawValue : rawValues) {
            parsedValues.push_back(stripQuotesCopy(rawValue));
        }
        q.insertRawRows.push_back(rawValues);
        q.insertRows.push_back(parsedValues);

        pos = closePos + 1;
        while (pos < valuesExpr.size() &&
               std::isspace(static_cast<unsigned char>(valuesExpr[pos]))) {
            ++pos;
        }
        if (pos < valuesExpr.size() && valuesExpr[pos] == ',') {
            ++pos;
        }
    }

    if (q.insertRows.empty()) {
        q.error = "INSERT requires at least one row";
        return q;
    }
    q.insertValues = q.insertRows.front();
    return q;
}

ParsedQuery Parser::parseSelect(const std::string &sql) {
    ParsedQuery q;
    q.type = QueryType::SELECT;

    const size_t fromPos = findKeywordOutside(sql, "FROM", std::string("SELECT").size());
    if (fromPos == std::string::npos) {
        q.error = "Invalid SELECT syntax";
        return q;
    }

    const std::string colsPart =
        trimCopy(sql.substr(std::string("SELECT").size(), fromPos - std::string("SELECT").size()));
    if (colsPart.empty()) {
        q.error = "SELECT requires at least one column";
        return q;
    }
    if (colsPart != "*") {
        q.selectCols = splitCommaAware(colsPart);
    }

    const size_t joinPos = findKeywordOutside(sql, "INNER JOIN", fromPos + std::string("FROM").size());
    const size_t wherePos = findKeywordOutside(sql, "WHERE", fromPos + std::string("FROM").size());
    const size_t orderPos = findKeywordOutside(sql, "ORDER BY", fromPos + std::string("FROM").size());

    size_t tableEnd = sql.size();
    for (size_t candidate : {joinPos, wherePos, orderPos}) {
        if (candidate != std::string::npos) {
            tableEnd = std::min(tableEnd, candidate);
        }
    }

    q.tableName = trimCopy(sql.substr(fromPos + std::string("FROM").size(),
                                      tableEnd - (fromPos + std::string("FROM").size())));
    if (q.tableName.empty()) {
        q.error = "Missing table name";
        return q;
    }

    if (joinPos != std::string::npos) {
        const size_t onPos = findKeywordOutside(sql, "ON", joinPos + std::string("INNER JOIN").size());
        if (onPos == std::string::npos) {
            q.error = "INNER JOIN requires an ON clause";
            return q;
        }

        size_t joinEnd = sql.size();
        for (size_t candidate : {wherePos, orderPos}) {
            if (candidate != std::string::npos && candidate > onPos) {
                joinEnd = std::min(joinEnd, candidate);
            }
        }

        q.join.active = true;
        q.join.rightTable = trimCopy(sql.substr(joinPos + std::string("INNER JOIN").size(),
                                                onPos - (joinPos + std::string("INNER JOIN").size())));
        if (q.join.rightTable.empty()) {
            q.error = "INNER JOIN requires a right-hand table";
            return q;
        }

        std::string lhs;
        std::string rhs;
        std::string condErr;
        const std::string joinExpr =
            trimCopy(sql.substr(onPos + std::string("ON").size(),
                                joinEnd - (onPos + std::string("ON").size())));
        if (!parseConditionExpr(joinExpr, lhs, q.join.op, rhs, condErr)) {
            q.error = condErr;
            return q;
        }

        auto lhsRef = splitQualifiedName(lhs);
        auto rhsRef = splitQualifiedName(rhs);
        if (!lhsRef.first.empty() && upperCopy(lhsRef.first) == upperCopy(q.join.rightTable) &&
            (rhsRef.first.empty() || upperCopy(rhsRef.first) == upperCopy(q.tableName))) {
            std::swap(lhsRef, rhsRef);
        }
        if (!lhsRef.first.empty() && upperCopy(lhsRef.first) != upperCopy(q.tableName)) {
            q.error = "Invalid left join column reference";
            return q;
        }
        if (!rhsRef.first.empty() && upperCopy(rhsRef.first) != upperCopy(q.join.rightTable)) {
            q.error = "Invalid right join column reference";
            return q;
        }
        q.join.leftCol = lhsRef.second;
        q.join.rightCol = rhsRef.second;
    }

    if (wherePos != std::string::npos) {
        size_t whereEnd = (orderPos == std::string::npos) ? sql.size() : orderPos;
        std::string whereErr;
        q.where.active = true;
        if (!parseConditionExpr(trimCopy(sql.substr(wherePos + std::string("WHERE").size(),
                                                    whereEnd - (wherePos + std::string("WHERE").size()))),
                                q.where.column, q.where.op, q.where.value, whereErr)) {
            q.error = whereErr;
            return q;
        }
        q.where.value = stripQuotesCopy(q.where.value);
    }

    if (orderPos != std::string::npos) {
        q.orderBy.active = true;
        const std::string orderExpr = trimCopy(sql.substr(orderPos + std::string("ORDER BY").size()));
        if (orderExpr.empty()) {
            q.error = "ORDER BY requires a column";
            return q;
        }

        std::istringstream ss(orderExpr);
        ss >> q.orderBy.column;
        std::string direction;
        if (ss >> direction) {
            const std::string upperDirection = upperCopy(direction);
            if (upperDirection == "DESC") {
                q.orderBy.ascending = false;
            } else if (upperDirection != "ASC") {
                q.error = "Unsupported ORDER BY direction";
                return q;
            }
            std::string extra;
            if (ss >> extra) {
                q.error = "Invalid ORDER BY clause";
                return q;
            }
        }
    }

    return q;
}

ParsedQuery Parser::parseDelete(const std::string &sql) {
    ParsedQuery q;
    q.type = QueryType::DELETE_ROWS;

    if (!startsWithKeyword(sql, "DELETE FROM")) {
        q.error = "Invalid DELETE syntax";
        return q;
    }

    q.tableName = trimCopy(sql.substr(std::string("DELETE FROM").size()));
    if (q.tableName.empty()) {
        q.error = "Missing table name";
        return q;
    }
    if (findKeywordOutside(q.tableName, "WHERE") != std::string::npos) {
        q.error = "DELETE with WHERE is not supported";
        return q;
    }
    return q;
}
