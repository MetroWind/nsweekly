#include <algorithm>
#include <map>
#include <spdlog/spdlog.h>
#include "game_data.hpp"

namespace
{
class Transaction
{
  public:
    explicit Transaction(SQLite &connection) : db(connection) {}
    E<void> begin()
    {
        DO_OR_RETURN(db.execute("BEGIN IMMEDIATE"));
        active = true;
        return {};
    }
    E<void> commit()
    {
        DO_OR_RETURN(db.execute("COMMIT"));
        active = false;
        return {};
    }
    ~Transaction()
    {
        if(active)
        {
            auto result = db.execute("ROLLBACK");
            if(!result)
            {
                spdlog::error("Game transaction rollback failed");
                if(!db.autocommit())
                {
                    db.invalidate();
                }
            }
        }
    }
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

  private:
    SQLite &db;
    bool active = false;
};

std::optional<std::string>
dateText(const std::optional<std::chrono::year_month_day> &date)
{
    if(date)
    {
        return gameDate(*date);
    }
    return {};
}
} // namespace

E<void> GameDataSqlite::initializeSchema()
{
    DO_OR_RETURN(db->execute("PRAGMA foreign_keys = ON"));
    ASSIGN_OR_RETURN(auto enabled, db->eval<int>("PRAGMA foreign_keys"));
    if(enabled.empty() || std::get<0>(enabled[0]) != 1)
    {
        return std::unexpected(runtimeError("Foreign keys unavailable"));
    }
    DO_OR_RETURN(db->execute(R"(
CREATE TABLE IF NOT EXISTS GameTracking
(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES Users(id),
    name TEXT COLLATE BINARY NOT NULL CHECK(length(name) > 0),
    status INTEGER NOT NULL CHECK(
        typeof(status) = 'integer' AND status IN (0, 1, 2, 3, 4)),
    completion INTEGER CHECK(completion IS NULL OR
        (typeof(completion) = 'integer' AND completion IN (0, 1, 2, 3, 4))),
    hours INTEGER CHECK(hours IS NULL OR
        (typeof(hours) = 'integer' AND hours BETWEEN 0 AND 2147483647)),
    start_date TEXT,
    end_date TEXT,
    notes TEXT,
    UNIQUE(user_id, name),
    CHECK(start_date IS NULL OR end_date IS NULL OR end_date >= start_date)
))"));
    DO_OR_RETURN(db->execute(R"(
CREATE TABLE IF NOT EXISTS GamePlatforms
(
    game_id INTEGER NOT NULL REFERENCES GameTracking(id) ON DELETE CASCADE,
    platform INTEGER NOT NULL CHECK(
        typeof(platform) = 'integer' AND platform IN (0, 1, 2, 3, 4)),
    PRIMARY KEY(game_id, platform)
))"));
    return db->execute(R"(
CREATE TABLE IF NOT EXISTS GameReviews
(
    game_id INTEGER PRIMARY KEY REFERENCES GameTracking(id) ON DELETE CASCADE,
    story REAL CHECK(story IS NULL OR
        (typeof(story) IN ('real', 'integer') AND story BETWEEN 1 AND 10)),
    gameplay REAL CHECK(gameplay IS NULL OR
        (typeof(gameplay) IN ('real', 'integer') AND gameplay BETWEEN 1 AND 10)),
    graphics REAL CHECK(graphics IS NULL OR
        (typeof(graphics) IN ('real', 'integer') AND graphics BETWEEN 1 AND 10)),
    audio REAL CHECK(audio IS NULL OR
        (typeof(audio) IN ('real', 'integer') AND audio BETWEEN 1 AND 10)),
    special REAL CHECK(special IS NULL OR
        (typeof(special) IN ('real', 'integer') AND special BETWEEN 1 AND 10)),
    text TEXT NOT NULL DEFAULT '',
    added INTEGER NOT NULL DEFAULT (CAST(strftime('%s', 'now') AS INTEGER)),
    updated INTEGER NOT NULL DEFAULT (CAST(strftime('%s', 'now') AS INTEGER))
))");
}

E<std::vector<GameRecord>> GameDataSqlite::listGames(const std::string &user)
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
SELECT g.id, g.name, g.status, g.completion, g.hours,
       g.start_date, g.end_date, g.notes, p.platform,
       typeof(g.status), typeof(g.completion),
       typeof(g.hours), typeof(p.platform)
FROM GameTracking g JOIN Users u ON u.id = g.user_id
LEFT JOIN GamePlatforms p ON p.game_id = g.id
WHERE u.name = ? ORDER BY g.name, g.id, p.platform
)"));
    DO_OR_RETURN(sql.bind(user));
    ASSIGN_OR_RETURN(
        auto rows,
        (db->eval<int64_t, std::string, int64_t, std::optional<int64_t>,
                  std::optional<std::string>, std::optional<std::string>,
                  std::optional<std::string>, std::optional<std::string>,
                  std::optional<int64_t>, std::string, std::string, std::string,
                  std::string>(std::move(sql))));
    std::map<int64_t, GameRecord> records;
    for(const auto &[id, name, status, completion, hours, start, end, notes,
                     platform, status_type, completion_type, hours_type,
                     platform_type] : rows)
    {
        if(status_type != "integer" ||
           (completion && completion_type != "integer") ||
           (hours && (hours_type != "integer" || hours->empty())) ||
           (start && start->empty()) || (end && end->empty()) ||
           (platform && platform_type != "integer"))
        {
            return std::unexpected(runtimeError("Invalid stored game type"));
        }
        if(id <= 0 || status < 0 || status > 4 ||
           (completion && (*completion < 0 || *completion > 4)) ||
           (platform && (*platform < 0 || *platform > 4)))
        {
            return std::unexpected(runtimeError("Invalid stored game enum/ID"));
        }
        if(!records.contains(id))
        {
            GameFields fields;
            fields.scalars = {
                {"name", name},
                {"status", std::string(STATUS_CHOICES[status].code)},
                {"completion",
                 completion ? std::string(COMPLETION_CHOICES[*completion].code)
                            : ""},
                {"hours", hours.value_or("")},
                {"start_date", start.value_or("")},
                {"end_date", end.value_or("")},
                {"notes", notes.value_or("")}};
            auto input = validateGameInput(fields);
            if(!input)
            {
                return std::unexpected(runtimeError("Invalid stored game"));
            }
            records.emplace(id, GameRecord{id, std::move(*input)});
        }
        auto &platforms = records.at(id).input.platforms;
        if(platform)
        {
            auto value = static_cast<GamePlatform>(*platform);
            if(std::ranges::find(platforms, value) == platforms.end())
            {
                platforms.push_back(value);
            }
        }
    }
    std::vector<GameRecord> games;
    for(auto &[id, record] : records)
    {
        std::ranges::sort(record.input.platforms);
        games.push_back(std::move(record));
    }
    std::ranges::sort(games,
                      [](const GameRecord &a, const GameRecord &b)
                      {
                          return std::tie(a.input.name, a.id) <
                                 std::tie(b.input.name, b.id);
                      });
    return games;
}

E<std::optional<GameRecord>> GameDataSqlite::getGame(const std::string &user,
                                                     int64_t id)
{
    ASSIGN_OR_RETURN(auto games, listGames(user));
    for(auto &game : games)
    {
        if(game.id == id)
        {
            return std::optional<GameRecord>(std::move(game));
        }
    }
    return std::optional<GameRecord>{};
}

E<GameRecord> GameDataSqlite::createGame(const std::string &user,
                                         const GameInput &input)
{
    ASSIGN_OR_RETURN(auto valid, validateGameInput(input));
    std::lock_guard lock(write_mutex);
    return save(user, 0, valid);
}

E<GameRecord> GameDataSqlite::updateGame(const std::string &user, int64_t id,
                                         const GameInput &input)
{
    ASSIGN_OR_RETURN(auto valid, validateGameInput(input));
    std::lock_guard lock(write_mutex);
    if(id <= 0)
    {
        return std::unexpected(httpError(404, "Game not found"));
    }
    return save(user, id, valid);
}

E<GameRecord> GameDataSqlite::save(const std::string &user, int64_t id,
                                   const GameInput &input)
{
    Transaction transaction(*db);
    DO_OR_RETURN(transaction.begin());
    if(id)
    {
        ASSIGN_OR_RETURN(auto old, getGame(user, id));
        if(!old)
        {
            return std::unexpected(httpError(404, "Game not found"));
        }
    }
    ASSIGN_OR_RETURN(auto duplicate, db->statementFromStr(R"(
SELECT g.id FROM GameTracking g JOIN Users u ON u.id = g.user_id
WHERE u.name = ? AND g.name = ? AND g.id != ?
)"));
    DO_OR_RETURN(duplicate.bind(user, input.name, id));
    ASSIGN_OR_RETURN(auto matches, db->eval<int64_t>(std::move(duplicate)));
    if(!matches.empty())
    {
        return std::unexpected(runtimeError("Game name already exists"));
    }
    const char *insert = R"(
INSERT INTO GameTracking
(name, status, completion, hours, start_date, end_date, notes, user_id)
SELECT ?, ?, ?, ?, ?, ?, ?, id FROM Users WHERE name = ? RETURNING id
)";
    const char *update = R"(
UPDATE GameTracking SET name=?, status=?, completion=?, hours=?,
start_date=?, end_date=?, notes=?
WHERE user_id=(SELECT id FROM Users WHERE name=?) AND id=? RETURNING id
)";
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(id ? update : insert));
    std::optional<int> completion;
    if(input.completion)
    {
        completion = static_cast<int>(*input.completion);
    }
    if(id)
    {
        DO_OR_RETURN(sql.bind(input.name, static_cast<int>(input.status),
                              completion, input.hours,
                              dateText(input.start_date),
                              dateText(input.end_date), input.notes, user, id));
    }
    else
    {
        DO_OR_RETURN(sql.bind(input.name, static_cast<int>(input.status),
                              completion, input.hours,
                              dateText(input.start_date),
                              dateText(input.end_date), input.notes, user));
    }
    ASSIGN_OR_RETURN(auto rows, db->eval<int64_t>(std::move(sql)));
    if(rows.empty())
    {
        return std::unexpected(httpError(404, "User not found"));
    }
    id = std::get<0>(rows.front());
    ASSIGN_OR_RETURN(auto clear, db->statementFromStr(R"(
DELETE FROM GamePlatforms WHERE game_id IN
(SELECT g.id FROM GameTracking g JOIN Users u ON u.id=g.user_id
 WHERE u.name=? AND g.id=?)
)"));
    DO_OR_RETURN(clear.bind(user, id));
    DO_OR_RETURN(db->execute(std::move(clear)));
    for(auto platform : input.platforms)
    {
        ASSIGN_OR_RETURN(auto add, db->statementFromStr(R"(
INSERT INTO GamePlatforms(game_id, platform)
SELECT g.id, ? FROM GameTracking g JOIN Users u ON u.id=g.user_id
WHERE u.name=? AND g.id=?
)"));
        DO_OR_RETURN(add.bind(static_cast<int>(platform), user, id));
        DO_OR_RETURN(db->execute(std::move(add)));
    }
    ASSIGN_OR_RETURN(auto saved, getGame(user, id));
    if(!saved)
    {
        return std::unexpected(runtimeError("Game readback failed"));
    }
    DO_OR_RETURN(transaction.commit());
    return std::move(*saved);
}

E<bool> GameDataSqlite::deleteGame(const std::string &user, int64_t id)
{
    std::lock_guard lock(write_mutex);
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
DELETE FROM GameTracking WHERE id=? AND
user_id=(SELECT id FROM Users WHERE name=?) RETURNING id
)"));
    DO_OR_RETURN(sql.bind(id, user));
    ASSIGN_OR_RETURN(auto rows, db->eval<int64_t>(std::move(sql)));
    return !rows.empty();
}
