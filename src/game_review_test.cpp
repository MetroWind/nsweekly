#include <gtest/gtest.h>
#include "game_data.hpp"
#include "storage_test_support.hpp"
#include "user_data.hpp"
#include "test_utils.hpp"

TEST(GameReviews, ValidatesFractionsMissingScoresAndText)
{
    GameFields fields;
    fields.scalars["gameplay"] = "8.5";
    ASSIGN_OR_FAIL(auto valid, validateReview(fields));
    EXPECT_EQ(valid.scores[1], 8.5);
    EXPECT_FALSE(valid.scores[0]);
    for(const auto *value : {"0", "11", "nan", "inf", "5oops"})
    {
        fields.scalars["story"] = value;
        EXPECT_FALSE(validateReview(fields));
    }
    fields.scalars["story"] = "1";
    fields.scalars["text"] = std::string("bad\0text", 8);
    EXPECT_FALSE(validateReview(fields));
}

TEST(GameReviews, OwnershipReplacementTimestampsAndCascade)
{
    TemporaryDatabase file;
    ASSIGN_OR_FAIL(auto users_db, SQLite::connectFile(file.path()));
    UserDataSqlite users(std::move(users_db));
    ASSERT_TRUE(users.initializeSchema());
    ASSERT_TRUE(users.ensureUser("alice"));
    ASSERT_TRUE(users.ensureUser("bob"));
    ASSIGN_OR_FAIL(auto connection, SQLite::connectFile(file.path()));
    GameDataSqlite data(std::move(connection));
    ASSERT_TRUE(data.initializeSchema());
    GameInput game;
    game.name = "Review Game";
    ASSIGN_OR_FAIL(auto tracked, data.createGame("alice", game));
    GameReviewInput review;
    review.scores = {10, 10, 8, 10, 10};
    review.text = "Review text";
    EXPECT_FALSE(data.saveReview("bob", tracked.id, review));
    EXPECT_FALSE(data.saveReview("alice", tracked.id + 1, review));
    ASSERT_TRUE(data.saveReview("alice", tracked.id, review));
    ASSIGN_OR_FAIL(auto rows, data.listReviews("alice"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].input.scores, review.scores);
    EXPECT_GT(rows[0].added, 0);
    const auto added = rows[0].added;
    ASSIGN_OR_FAIL(auto foreign, data.listReviews("bob"));
    EXPECT_TRUE(foreign.empty());
    ASSIGN_OR_FAIL(auto removed, data.deleteReview("bob", tracked.id));
    EXPECT_FALSE(removed);
    review.scores[2].reset();
    ASSERT_TRUE(data.saveReview("alice", tracked.id, review));
    ASSIGN_OR_FAIL(rows, data.listReviews("alice"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].added, added);
    EXPECT_GE(rows[0].updated, added);
    EXPECT_FALSE(rows[0].input.scores[2]);
    game.name = "Renamed";
    ASSERT_TRUE(data.updateGame("alice", tracked.id, game));
    ASSERT_TRUE(data.deleteReview("alice", tracked.id));
    ASSIGN_OR_FAIL(auto still_tracked, data.getGame("alice", tracked.id));
    EXPECT_TRUE(still_tracked);
    ASSERT_TRUE(data.saveReview("alice", tracked.id, review));
    ASSERT_TRUE(data.deleteGame("alice", tracked.id));
    ASSIGN_OR_FAIL(rows, data.listReviews("alice"));
    EXPECT_TRUE(rows.empty());
    ASSIGN_OR_FAIL(auto raw, SQLite::connectFile(file.path()));
    ASSIGN_OR_FAIL(auto columns, raw->eval<std::string>(
        "SELECT name FROM pragma_table_info('GameReviews')"));
    for(const auto &[name] : columns)
    {
        EXPECT_NE(name, "overall");
        EXPECT_FALSE(name.ends_with("_comment"));
    }
    ASSIGN_OR_FAIL(auto types, raw->eval<std::string>(
        "SELECT type FROM pragma_table_info('GameReviews') "
        "WHERE name IN ('added', 'updated')"));
    ASSERT_EQ(types.size(), 2);
    EXPECT_EQ(std::get<0>(types[0]), "INTEGER");
    EXPECT_EQ(std::get<0>(types[1]), "INTEGER");
}
