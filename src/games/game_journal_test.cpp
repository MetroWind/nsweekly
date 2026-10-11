#include <gtest/gtest.h>
#include "games/game_data.hpp"
#include "games/game_markdown.hpp"
#include "storage_test_support.hpp"
#include "user_data.hpp"
#include "test_utils.hpp"

TEST(GameJournal, ValidatesWithoutChangingSource)
{
    GameFields fields;
    EXPECT_FALSE(validateJournal(fields));
    for(const auto &text : {std::string(" \t\r\n"), std::string("bad\0text", 8),
        std::string("\xff"), std::string(JOURNAL_TEXT_LIMIT + 1, 'a')})
    {
        fields.scalars["text"] = text;
        EXPECT_FALSE(validateJournal(fields));
    }
    for(const auto &text : {std::string("  日本語\r\n![image](url)  \n"),
        std::string(JOURNAL_TEXT_LIMIT, 'a')})
    {
        fields.scalars["text"] = text;
        ASSIGN_OR_FAIL(auto valid, validateJournal(fields));
        EXPECT_EQ(valid.text, text);
    }
}

TEST(GameJournal, OwnershipOrderTimestampsAndCascades)
{
    TemporaryDatabase file;
    ASSIGN_OR_FAIL(auto connection, SQLite::connectFile(file.path()));
    UserDataSqlite users(std::move(connection));
    ASSERT_TRUE(users.initializeSchema());
    ASSERT_TRUE(users.ensureUser("alice"));
    ASSERT_TRUE(users.ensureUser("bob"));
    ASSIGN_OR_FAIL(connection, SQLite::connectFile(file.path()));
    GameDataSqlite data(std::move(connection));
    ASSERT_TRUE(data.initializeSchema());
    ASSERT_TRUE(data.initializeSchema());
    GameInput input;
    input.name = "First";
    ASSIGN_OR_FAIL(auto game, data.createGame("alice", input));
    input.name = "Second";
    ASSIGN_OR_FAIL(auto second, data.createGame("alice", input));
    GameJournalInput source{"  日本語\r\nsource  \n"};
    EXPECT_FALSE(data.createJournalEntry("bob", game.id, source));
    EXPECT_FALSE(data.createJournalEntry("missing", game.id, source));
    EXPECT_FALSE(data.createJournalEntry("alice", game.id, {" \n"}));
    ASSIGN_OR_FAIL(auto first, data.createJournalEntry("alice", game.id, source));
    ASSIGN_OR_FAIL(auto next, data.createJournalEntry("alice", game.id, source));
    EXPECT_GT(first.added, 0);
    ASSIGN_OR_FAIL(auto missing, data.getJournalEntry("bob", game.id, first.id));
    EXPECT_FALSE(missing);
    ASSIGN_OR_FAIL(missing, data.getJournalEntry("alice", second.id, first.id));
    EXPECT_FALSE(missing);
    EXPECT_FALSE(data.updateJournalEntry("alice", second.id, first.id, source));
    EXPECT_FALSE(data.updateJournalEntry("bob", game.id, first.id, source));
    ASSIGN_OR_FAIL(auto removed,
        data.deleteJournalEntry("alice", second.id, first.id));
    EXPECT_FALSE(removed);
    ASSIGN_OR_FAIL(auto updated,
        data.updateJournalEntry("alice", game.id, first.id, {"updated"}));
    EXPECT_EQ(updated.added, first.added);
    EXPECT_GE(updated.updated, updated.added);
    ASSIGN_OR_FAIL(auto rows, data.listJournal("alice", game.id));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].id, first.id);
    EXPECT_EQ(rows[1].id, next.id);
    EXPECT_EQ(rows[1].input.text, source.text);
    ASSERT_TRUE(data.deleteJournalEntry("alice", game.id, next.id));
    ASSIGN_OR_FAIL(auto last, data.createJournalEntry("alice", game.id, source));
    EXPECT_GT(last.id, next.id);
    ASSERT_TRUE(data.saveReview("alice", game.id, {}));
    ASSERT_TRUE(data.deleteGame("alice", game.id));
    ASSIGN_OR_FAIL(rows, data.listJournal("alice", game.id));
    EXPECT_TRUE(rows.empty());
    EXPECT_TRUE(data.listReviews("alice")->empty());
    EXPECT_FALSE(data.createJournalEntry("alice", game.id, source));
    ASSIGN_OR_FAIL(auto raw, SQLite::connectFile(file.path()));
    ASSERT_TRUE(raw->execute("DROP INDEX GameJournalEntriesOrder"));
    ASSERT_TRUE(data.initializeSchema());
    ASSIGN_OR_FAIL(auto indexes, raw->eval<std::string>(
        "SELECT name FROM sqlite_master WHERE name='GameJournalEntriesOrder'"));
    EXPECT_EQ(indexes.size(), 1);
}

TEST(GameJournal, AttachmentLinksUseActualMarkdownRenderer)
{
    for(const auto *url : {"/attachment/hash/a%5Bb%5D%28c%29.png",
        "/attachment/hash/%E6%97%A5%20%22.png"})
    {
        ASSIGN_OR_FAIL(auto html, renderGameMarkdown(
            std::string("![a\\[b\\](c) 日本 \\\\](<") + url + ">)"));
        EXPECT_NE(html.find("<img"), std::string::npos);
        EXPECT_NE(html.find(url), std::string::npos);
    }
}
