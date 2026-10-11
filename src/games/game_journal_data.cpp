#include "games/game_data.hpp"

namespace
{
template<typename Statement>
E<std::vector<GameJournalRecord>> journalRows(SQLite &db, Statement sql)
{
    ASSIGN_OR_RETURN(auto rows,
        (db.eval<int64_t, int64_t, std::string, int64_t, int64_t>(
            std::move(sql))));
    std::vector<GameJournalRecord> result;
    for(auto &[id, game_id, text, added, updated] : rows)
    {
        result.push_back({id, game_id, {std::move(text)}, added, updated});
    }
    return result;
}
} // namespace

E<std::vector<GameJournalRecord>> GameDataSqlite::listJournal(
    const std::string &user, int64_t game_id)
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
SELECT j.id, j.game_id, j.text, j.added, j.updated
FROM GameJournalEntries j JOIN GameTracking g ON g.id=j.game_id
JOIN Users u ON u.id=g.user_id WHERE u.name=? AND g.id=?
ORDER BY j.added, j.id
)"));
    DO_OR_RETURN(sql.bind(user, game_id));
    ASSIGN_OR_RETURN(auto rows, journalRows(*db, std::move(sql)));
    return rows;
}

E<std::optional<GameJournalRecord>> GameDataSqlite::getJournalEntry(
    const std::string &user, int64_t game_id, int64_t entry_id)
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
SELECT j.id, j.game_id, j.text, j.added, j.updated
FROM GameJournalEntries j JOIN GameTracking g ON g.id=j.game_id
JOIN Users u ON u.id=g.user_id WHERE u.name=? AND g.id=?
AND j.id=?
)"));
    DO_OR_RETURN(sql.bind(user, game_id, entry_id));
    ASSIGN_OR_RETURN(auto rows, journalRows(*db, std::move(sql)));
    if(rows.empty())
    {
        return std::optional<GameJournalRecord>{};
    }
    return std::optional<GameJournalRecord>{std::move(rows.front())};
}

E<GameJournalRecord> GameDataSqlite::createJournalEntry(
    const std::string &user, int64_t game_id,
    const GameJournalInput &input)
{
    GameFields fields;
    fields.scalars["text"] = input.text;
    ASSIGN_OR_RETURN(auto valid, validateJournal(fields));
    std::lock_guard lock(write_mutex);
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
INSERT INTO GameJournalEntries(text, game_id)
SELECT ?, g.id FROM GameTracking g JOIN Users u ON u.id=g.user_id
WHERE u.name=? AND g.id=?
RETURNING id, game_id, text, added, updated
)"));
    DO_OR_RETURN(sql.bind(valid.text, user, game_id));
    ASSIGN_OR_RETURN(auto rows, journalRows(*db, std::move(sql)));
    if(rows.empty())
    {
        return std::unexpected(httpError(404, "Journal parent or entry missing"));
    }
    return std::move(rows.front());
}

E<GameJournalRecord> GameDataSqlite::updateJournalEntry(
    const std::string &user, int64_t game_id,
    int64_t entry_id, const GameJournalInput &input)
{
    GameFields fields;
    fields.scalars["text"] = input.text;
    ASSIGN_OR_RETURN(auto valid, validateJournal(fields));
    std::lock_guard lock(write_mutex);
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
UPDATE GameJournalEntries SET text=?,
updated=CAST(strftime('%s', 'now') AS INTEGER)
WHERE game_id IN (SELECT g.id FROM GameTracking g
JOIN Users u ON u.id=g.user_id WHERE u.name=? AND g.id=?) AND id=?
RETURNING id, game_id, text, added, updated
)"));
    DO_OR_RETURN(sql.bind(valid.text, user, game_id, entry_id));
    ASSIGN_OR_RETURN(auto rows, journalRows(*db, std::move(sql)));
    if(rows.empty())
    {
        return std::unexpected(httpError(404, "Journal parent or entry missing"));
    }
    return std::move(rows.front());
}

E<bool> GameDataSqlite::deleteJournalEntry(
    const std::string &user, int64_t game_id, int64_t entry_id)
{
    std::lock_guard lock(write_mutex);
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
DELETE FROM GameJournalEntries WHERE id=? AND game_id IN
(SELECT g.id FROM GameTracking g JOIN Users u ON u.id=g.user_id
 WHERE u.name=? AND g.id=?) RETURNING id
)"));
    DO_OR_RETURN(sql.bind(entry_id, user, game_id));
    ASSIGN_OR_RETURN(auto rows, db->eval<int64_t>(std::move(sql)));
    return !rows.empty();
}
