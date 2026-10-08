#include "game_data.hpp"

E<std::vector<GameReviewRecord>> GameDataSqlite::listReviews(
    const std::string &user)
{
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
SELECT r.game_id, r.story, r.gameplay, r.graphics, r.audio, r.special,
       r.text, r.added, r.updated
FROM GameReviews r JOIN GameTracking g ON g.id = r.game_id
JOIN Users u ON u.id = g.user_id WHERE u.name = ? ORDER BY g.name, g.id
)"));
    DO_OR_RETURN(sql.bind(user));
    ASSIGN_OR_RETURN(auto rows,
        (db->eval<int64_t, std::optional<double>, std::optional<double>,
                  std::optional<double>, std::optional<double>,
                  std::optional<double>, std::string,
                  int64_t, int64_t>(std::move(sql))));
    std::vector<GameReviewRecord> result;
    for(const auto &[id, story, gameplay, graphics, audio, special,
                     text, added, updated] : rows)
    {
        GameReviewInput input{{story, gameplay, graphics, audio, special},
            text};
        ASSIGN_OR_RETURN(auto valid, validateReview(reviewFields(input)));
        result.push_back({id, std::move(valid), added, updated});
    }
    return result;
}

E<void> GameDataSqlite::saveReview(const std::string &user, int64_t game_id,
                                  const GameReviewInput &input)
{
    ASSIGN_OR_RETURN(auto valid, validateReview(reviewFields(input)));
    std::lock_guard lock(write_mutex);
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
INSERT INTO GameReviews
(game_id, story, gameplay, graphics, audio, special, text)
SELECT g.id, ?, ?, ?, ?, ?, ?
FROM GameTracking g JOIN Users u ON u.id = g.user_id
WHERE g.id = ? AND u.name = ?
ON CONFLICT(game_id) DO UPDATE SET
 story = excluded.story, gameplay = excluded.gameplay,
 graphics = excluded.graphics, audio = excluded.audio,
 special = excluded.special, text = excluded.text,
 updated = CAST(strftime('%s', 'now') AS INTEGER)
RETURNING game_id
)"));
    DO_OR_RETURN(sql.bind(valid.scores[0], valid.scores[1], valid.scores[2],
        valid.scores[3], valid.scores[4], valid.text,
        game_id, user));
    ASSIGN_OR_RETURN(auto saved, db->eval<int64_t>(std::move(sql)));
    if(saved.empty())
    {
        return std::unexpected(httpError(404, "Game not found"));
    }
    return {};
}

E<bool> GameDataSqlite::deleteReview(const std::string &user, int64_t game_id)
{
    std::lock_guard lock(write_mutex);
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
DELETE FROM GameReviews WHERE game_id IN
(SELECT g.id FROM GameTracking g JOIN Users u ON u.id = g.user_id
 WHERE g.id = ? AND u.name = ?) RETURNING game_id
)"));
    DO_OR_RETURN(sql.bind(game_id, user));
    ASSIGN_OR_RETURN(auto removed, db->eval<int64_t>(std::move(sql)));
    return !removed.empty();
}

E<bool> GameDataSqlite::importReview(const std::string &user, int64_t game_id,
    const GameReviewInput &input, int64_t added, int64_t updated)
{
    ASSIGN_OR_RETURN(auto valid, validateReview(reviewFields(input)));
    if(updated < added)
    {
        return std::unexpected(runtimeError("Update precedes addition"));
    }
    std::lock_guard lock(write_mutex);
    ASSIGN_OR_RETURN(auto sql, db->statementFromStr(R"(
INSERT INTO GameReviews
(game_id, story, gameplay, graphics, audio, special, text, added, updated)
SELECT g.id, ?, ?, ?, ?, ?, ?, ?, ?
FROM GameTracking g JOIN Users u ON u.id = g.user_id
WHERE g.id = ? AND u.name = ?
ON CONFLICT(game_id) DO NOTHING
RETURNING game_id
)"));
    DO_OR_RETURN(sql.bind(valid.scores[0], valid.scores[1], valid.scores[2],
        valid.scores[3], valid.scores[4], valid.text, added, updated,
        game_id, user));
    ASSIGN_OR_RETURN(auto saved, db->eval<int64_t>(std::move(sql)));
    if(!saved.empty())
    {
        return true;
    }
    ASSIGN_OR_RETURN(auto game, getGame(user, game_id));
    if(!game)
    {
        return std::unexpected(httpError(404, "Game not found"));
    }
    return false;
}
