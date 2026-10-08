#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <sstream>
#include "game_import.hpp"
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

class ReviewMigration : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSIGN_OR_FAIL(auto user_db, SQLite::connectFile(file.path()));
        UserDataSqlite users(std::move(user_db));
        ASSERT_TRUE(users.initializeSchema());
        ASSERT_TRUE(users.ensureUser("alice"));
        ASSERT_TRUE(users.ensureUser("bob"));
        ASSIGN_OR_FAIL(auto connection, SQLite::connectFile(file.path()));
        data = std::make_unique<GameDataSqlite>(std::move(connection));
        ASSERT_TRUE(data->initializeSchema());
        GameInput game;
        game.name = "First";
        ASSIGN_OR_FAIL(auto first, data->createGame("alice", game));
        first_id = first.id;
        game.name = "Second";
        ASSERT_TRUE(data->createGame("alice", game));
    }
    TemporaryDatabase file;
    std::unique_ptr<GameDataSqlite> data;
    int64_t first_id = 0;
};

TEST_F(ReviewMigration, DatesDuplicatesIgnoredColumnsAndNoOverwrite)
{
    const std::string csv =
        "Game,Story/Lore,Game Play,Graphics,Audio,Special,Overall,,Addition,"
        "Update,Hours,Text export,Review\r\n"
        " First ,10,9.5,8,10,9,not-a-score,,2024-02-29,,bad,ignored,"
        "\"%code{Review}\nwith, quotes \"\"here\"\"\"\r\n"
        "First,1,1,1,1,1,,,,,,,\r\n"
        "Second,,,,,,,,2022-07-21,2022-07-22,,,\r\n"
        ",,,,,,,,,,,,\r\n";
    std::ostringstream report;
    EXPECT_EQ(importReviewsCsv(csv, "alice", *data, report), 0) << report.str();
    EXPECT_THAT(report.str(), ::testing::HasSubstr("inserted=2"));
    EXPECT_THAT(report.str(), ::testing::HasSubstr("duplicate-in-file=1"));
    EXPECT_THAT(report.str(), ::testing::HasSubstr("blank=1"));
    ASSIGN_OR_FAIL(auto rows, data->listReviews("alice"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].input.scores[1], 9.5);
    EXPECT_TRUE(rows[0].input.text.empty());
    EXPECT_EQ(rows[0].added, 1709164800);
    EXPECT_EQ(rows[0].updated, rows[0].added);
    EXPECT_EQ(rows[1].added, 1658361600);
    EXPECT_EQ(rows[1].updated, 1658448000);
    EXPECT_FALSE(rows[1].input.scores[0]);
    GameReviewInput changed;
    changed.scores.fill(3);
    changed.text = "Keep the existing review text";
    ASSERT_TRUE(data->saveReview("alice", first_id, changed));
    const auto updated = data->listReviews("alice")->at(0).updated;
    report.str("");
    EXPECT_EQ(importReviewsCsv(csv, "alice", *data, report), 0);
    EXPECT_THAT(report.str(), ::testing::HasSubstr("inserted=0"));
    EXPECT_THAT(report.str(), ::testing::HasSubstr("duplicate-in-database=2"));
    ASSIGN_OR_FAIL(rows, data->listReviews("alice"));
    EXPECT_EQ(rows[0].input.scores[0], 3);
    EXPECT_EQ(rows[0].input.text, changed.text);
    EXPECT_EQ(rows[0].updated, updated);
    EXPECT_FALSE(data->importReview("bob", first_id, changed, 1, 2));
    EXPECT_FALSE(data->importReview("alice", first_id, changed, 2, 1));
}

TEST_F(ReviewMigration, ValidatesWholeFileBeforeWriting)
{
    const std::string header =
        "Game,Story/Lore,Gameplay,Graphics,Audio,Special,Addition,Update\n";
    for(const auto &bad : {
        "Missing,5,5,5,5,5,,\n", "Second,0,5,5,5,5,,\n",
        "Second,5,5,5,5,5,2024-02-30,\n",
        "Second,5,5,5,5,5,2024-03-01,2024-02-29\n",
        "Second,5,5,5,5,5,,2024-02-29\n", "Second,5\n"})
    {
        std::ostringstream report;
        EXPECT_EQ(importReviewsCsv(header + "First,5,5,5,5,5,,\n" + bad,
            "alice", *data, report), 2) << report.str();
        EXPECT_TRUE(data->listReviews("alice")->empty());
    }
    std::ostringstream report;
    EXPECT_EQ(importReviewsCsv("Game,Game\nFirst,First\n",
        "alice", *data, report), 2);
    EXPECT_EQ(importReviewsCsv("Game\nFirst\n", "alice", *data, report), 2);
    EXPECT_EQ(importReviewsCsv(header + "First,5,5,5,5,5,,\n",
        "bob", *data, report), 2);
}

TEST_F(ReviewMigration, StorageFailureReportsCommittedRowsAndRerunResumes)
{
    ASSIGN_OR_FAIL(auto db, SQLite::connectFile(file.path()));
    ASSERT_TRUE(db->execute(R"(
CREATE TRIGGER FailReview BEFORE INSERT ON GameReviews
WHEN NEW.game_id != 1 BEGIN SELECT RAISE(ABORT, 'test failure'); END
)"));
    const std::string csv =
        "Game,Story/Lore,Game Play,Graphics,Audio,Special\n"
        "First,5,5,5,5,5\nSecond,6,6,6,6,6\n";
    std::ostringstream report;
    EXPECT_EQ(importReviewsCsv(csv, "alice", *data, report), 4);
    EXPECT_THAT(report.str(), ::testing::HasSubstr("1 inserts committed"));
    EXPECT_EQ(data->listReviews("alice")->size(), 1);
    ASSERT_TRUE(db->execute("DROP TRIGGER FailReview"));
    EXPECT_EQ(importReviewsCsv(csv, "alice", *data, report), 0);
    EXPECT_EQ(data->listReviews("alice")->size(), 2);
}
