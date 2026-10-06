#include "user_data.hpp"
#include "user_sql.hpp"

E<void> UserDataSqlite::initializeSchema()
{
    return initializeUsers(*db);
}

E<std::optional<int64_t>> UserDataSqlite::getUserID(
    const std::string& name) const
{
    return ::getUserID(*db, name);
}

E<int64_t> UserDataSqlite::createUser(const std::string& name) const
{
    return ::createUser(*db, name);
}

E<void> UserDataSqlite::ensureUser(const std::string& name) const
{
    return ::ensureUser(*db, name);
}
