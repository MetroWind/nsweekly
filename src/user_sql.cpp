#include "user_sql.hpp"

E<void> initializeUsers(SQLite& db)
{
    return db.execute("CREATE TABLE IF NOT EXISTS Users "
                      "(id INTEGER PRIMARY KEY ASC, name TEXT UNIQUE);");
}

E<void> ensureUser(SQLite& db, const std::string& name)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(
        "INSERT INTO Users(name) VALUES (?) ON CONFLICT(name) DO NOTHING;"));
    DO_OR_RETURN(sql.bind(name));
    return db.execute(std::move(sql));
}

E<std::optional<int64_t>>
getUserID(SQLite& db, const std::string& name)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(
        "SELECT id FROM Users WHERE name = ?;"));
    DO_OR_RETURN(sql.bind(name));
    ASSIGN_OR_RETURN(std::vector<std::tuple<int64_t>> result,
                     db.eval<int64_t>(std::move(sql)));
    if(result.empty())
    {
        return std::nullopt;
    }
    if(result.size() > 1)
    {
        return std::unexpected(runtimeError(
            std::format("Found multiple IDs for user {}", name)));
    }
    return std::get<0>(result[0]);
}

E<int64_t> createUser(SQLite& db, const std::string& name)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(
        "INSERT INTO Users (name) VALUES (?) RETURNING id;"));
    DO_OR_RETURN(sql.bind(name));
    ASSIGN_OR_RETURN(auto rows, db.eval<int64_t>(std::move(sql)));
    if(rows.size() != 1)
    {
        return std::unexpected(runtimeError("Missing inserted user ID"));
    }
    return std::get<0>(rows[0]);
}
