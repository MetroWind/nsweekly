#pragma once

#include <memory>
#include <tuple>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

#include <sqlite3.h>

#include "error.hpp"
#include "utils.hpp"

// This is a type that indicates no value should return from a SQL
// statement.
class NullValue {};

class SQLite
{
public:
    SQLite() = default;
    ~SQLite();
    SQLite(const SQLite&) = delete;
    SQLite& operator=(const SQLite&) = delete;

    static E<std::unique_ptr<SQLite>>
    connectFile(const std::string& db_file);
    static E<std::unique_ptr<SQLite>> connectMemory();

    template<typename... Types>
    E<std::vector<std::tuple<Types...>>> eval(const char* sql_code) const;
    template<typename... Types>
    E<std::vector<std::tuple<Types...>>> eval(const std::string& sql_code) const
    {
        return eval<Types...>(sql_code.c_str());
    }

    E<void> execute(const char* sql_code) const;
    E<void> execute(const std::string& sql_code) const
    {
        return execute(sql_code.c_str());
    }

private:
    sqlite3* db = nullptr;
    void clear();
};

namespace internal
{
// A simple RAII wrapper of sqlite3_stmt*.
class SQLiteStatement
{
public:
    SQLiteStatement() = default;
    SQLiteStatement(const SQLiteStatement&) = delete;
    SQLiteStatement& operator=(const SQLiteStatement&) = delete;
    SQLiteStatement(SQLiteStatement&& rhs)
    {
        std::swap(sql, rhs.sql);
    }
    SQLiteStatement& operator=(SQLiteStatement&& rhs)
    {
        std::swap(sql, rhs.sql);
        return *this;
    }

    ~SQLiteStatement()
    {
        if(sql != nullptr)
        {
            sqlite3_finalize(sql);
        }
    }

    static E<SQLiteStatement> fromStr(sqlite3* db, const char* expr)
    {
        SQLiteStatement s;
        int code = sqlite3_prepare_v2(db, expr, -1, &s.sql, nullptr);
        if(code != SQLITE_OK)
        {
            return std::unexpected(runtimeError(std::format(
                "Invalid statement '{}': {}", expr, sqlite3_errstr(code))));
        }
        return E<SQLiteStatement>{std::in_place, std::move(s)};
    }

    const sqlite3_stmt*const& data() const { return sql; }
    sqlite3_stmt*& data() { return sql; }

private:
    sqlite3_stmt* sql = nullptr;
};

template<typename T>
inline void getValue(SQLiteStatement&, int, T&)
{
    static_assert(false, "Invalid type of sqlite column");
}

template<>
inline void getValue(SQLiteStatement& sql, int i, int64_t& x)
{
    x = sqlite3_column_int64(sql.data(), i);
}

template<>
inline void getValue(SQLiteStatement& sql, int i, int& x)
{
    x = sqlite3_column_int(sql.data(), i);
}

template<>
inline void getValue(SQLiteStatement& sql, int i, double& x)
{
    x = sqlite3_column_double(sql.data(), i);
}

template<>
inline void getValue(SQLiteStatement& sql, int i, std::string& s)
{
    const unsigned char* raw = sqlite3_column_text(sql.data(), i);
    s = reinterpret_cast<const char*>(raw);
}

template<typename T, typename T1, typename... Types>
inline std::tuple<T, T1, Types...> getRowInternal(SQLiteStatement& sql, int i)
{
    std::tuple<T> result;
    getValue(sql, i, std::get<0>(result));
    return std::tuple_cat(result, getRowInternal<T1, Types...>(sql, i+1));
}

template<typename T>
inline std::tuple<T> getRowInternal(SQLiteStatement& sql, int i)
{
    std::tuple<T> result;
    getValue(sql, i, std::get<0>(result));
    return result;
}

template<typename T, typename... Types>
inline std::tuple<T, Types...> getRow(SQLiteStatement& sql)
{
    return getRowInternal<T, Types...>(sql, 0);
}

} // namespace internal

template<typename... Types>
E<std::vector<std::tuple<Types...>>> SQLite::eval(const char* sql_code) const
{
    ASSIGN_OR_RETURN(auto sql, internal::SQLiteStatement::fromStr(db, sql_code));
    std::vector<std::tuple<Types...>> result;
    while(true)
    {
        int code = sqlite3_step(sql.data());
        switch(code)
        {
        case SQLITE_DONE:
            return result;
        case SQLITE_ROW:
            result.push_back(internal::getRow<Types...>(sql));
            break;
        case SQLITE_BUSY:
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return std::unexpected(runtimeError(sqlite3_errstr(code)));
        case SQLITE_ERROR:
        case SQLITE_MISUSE:
            return std::unexpected(runtimeError(std::string(
                "Failed to evaluate SQL: ") + sqlite3_errstr(code)));
        default:
            return std::unexpected(runtimeError(std::string(
                "Unexpected return code when evaluating SQL: ") +
                sqlite3_errstr(code)));
        }
    }
    return result;
}
