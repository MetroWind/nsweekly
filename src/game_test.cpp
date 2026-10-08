#include <future>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "game.hpp"
#include "game_data.hpp"
#include "game_import.hpp"
#include "game_markdown.hpp"
#include "csv_reader.hpp"
#include "storage_test_support.hpp"
#include "user_data.hpp"
#include "test_utils.hpp"

using ::testing::HasSubstr;

TEST(GameValidation, PreservesMissingZeroAndIndependentChoices)
{
    GameFields fields{{{"name", "  Hades  "}, {"status", "now_playing"}}, {}};
    ASSIGN_OR_FAIL(auto input, validateGameInput(fields));
    EXPECT_EQ(input.name, "Hades");
    EXPECT_FALSE(input.hours);
    EXPECT_FALSE(input.completion);
    fields.scalars["hours"] = "00000";
    fields.scalars["notes"] = " \n  %code{Hello}\n  world  \n";
    fields.platforms = {"ps_5", "pc", "switch_2", "pc"};
    for(const auto &status : STATUS_CHOICES)
    {
        for(const auto &completion : COMPLETION_CHOICES)
        {
            fields.scalars["status"] = status.code;
            fields.scalars["completion"] = completion.code;
            ASSIGN_OR_FAIL(auto value, validateGameInput(fields));
            EXPECT_EQ(value.hours, 0);
            EXPECT_EQ(value.notes, "%code{Hello}\n  world");
            EXPECT_EQ(value.platforms,
                      (std::vector{GamePlatform::PC, GamePlatform::SWITCH_2,
                                   GamePlatform::PS_5}));
        }
    }
}

TEST(GameValidation, CollectsErrorsAndRejectsInvalidHours)
{
    for(const auto &hours :
        {"-1", "1.2", "2147483648", "1e3", "1x", "NaN", "+1", "1,000"})
    {
        GameFields fields{{{"name", " "},
                           {"status", "wrong"},
                           {"hours", hours},
                           {"completion", "unknown"}},
                          {"bad"}};
        auto result = validateGameInput(fields);
        ASSERT_FALSE(result);
        auto error = result.error().as<GameValidationError>();
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(error->fields.size(), 5);
    }
    GameInput input;
    input.name = "Game";
    input.hours = 2147483647;
    EXPECT_TRUE(validateGameInput(input));
}

TEST(GameValidation, CalendarRulesAndClearing)
{
    GameFields fields{{{"name", "Game"},
                       {"status", "done"},
                       {"start_date", "2024-02-29"},
                       {"end_date", "2024-02-28"}},
                      {}};
    auto invalid = validateGameInput(fields);
    ASSERT_FALSE(invalid);
    EXPECT_TRUE(
        invalid.error().as<GameValidationError>()->fields.contains("end_date"));
    fields.scalars["end_date"] = "2024-02-29";
    EXPECT_TRUE(validateGameInput(fields));
    fields.scalars["start_date"] = "";
    EXPECT_TRUE(validateGameInput(fields));
    for(const auto &date : {"2023-02-29", "0000-01-01", "2024-2-29",
                            "2024-01-01junk", "10000-01-01"})
    {
        fields.scalars["start_date"] = date;
        EXPECT_FALSE(validateGameInput(fields));
    }
    fields.scalars["start_date"] = "";
    fields.scalars["notes"] = " \n \t";
    ASSIGN_OR_FAIL(auto cleared, validateGameInput(fields));
    EXPECT_FALSE(cleared.notes);
}

TEST(GameValidation, TextEncodingAndEscaping)
{
    EXPECT_TRUE(validGameText("é 🎮"));
    EXPECT_FALSE(validGameText(std::string("a\0b", 3)));
    EXPECT_FALSE(validGameText("\xc0\x80"));
    EXPECT_FALSE(validGameText("\xed\xa0\x80"));
    EXPECT_FALSE(validGameText("\xf4\x90\x80\x80"));
    EXPECT_EQ(gameEscape("</textarea>\"&'"),
              "&lt;/textarea&gt;&quot;&amp;&apos;");
}

class GameStorage : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSIGN_OR_FAIL(auto connection, SQLite::connectFile(file.path()));
        db = connection.get();
        users =
            std::make_unique<UserDataSqlite>(*SQLite::connectFile(file.path()));
        ASSERT_TRUE(users->initializeSchema());
        ASSERT_TRUE(users->ensureUser("alice"));
        ASSERT_TRUE(users->ensureUser("bob"));
        games = std::make_unique<GameDataSqlite>(std::move(connection));
        ASSERT_TRUE(games->initializeSchema());
    }
    TemporaryDatabase file;
    SQLite *db = nullptr;
    std::unique_ptr<UserDataSqlite> users;
    std::unique_ptr<GameDataSqlite> games;
};

TEST_F(GameStorage, OwnerCrudUniquenessAndNoIdReuse)
{
    GameInput input;
    input.name = " Hades ";
    input.platforms = {GamePlatform::PS_5, GamePlatform::PC};
    input.hours = 0;
    input.completion = GameCompletion::NOT_STARTED;
    ASSIGN_OR_FAIL(auto created, games->createGame("alice", input));
    EXPECT_EQ(created.input.name, "Hades");
    EXPECT_FALSE(games->createGame("alice", input));
    EXPECT_TRUE(games->createGame("bob", input));
    ASSIGN_OR_FAIL(auto absent, games->getGame("bob", created.id));
    EXPECT_FALSE(absent);
    EXPECT_FALSE(games->updateGame("bob", created.id, input));
    ASSIGN_OR_FAIL(auto forbidden, games->deleteGame("bob", created.id));
    EXPECT_FALSE(forbidden);
    input.name = "hades";
    EXPECT_TRUE(games->createGame("alice", input));
    input.name = "Hades  II";
    input.platforms.clear();
    input.hours.reset();
    input.completion.reset();
    ASSIGN_OR_FAIL(auto edited, games->updateGame("alice", created.id, input));
    EXPECT_TRUE(edited.input.platforms.empty());
    EXPECT_FALSE(edited.input.hours);
    EXPECT_FALSE(edited.input.completion);
    ASSERT_TRUE(*games->deleteGame("alice", created.id));
    ASSIGN_OR_FAIL(auto fresh, games->createGame("alice", input));
    EXPECT_GT(fresh.id, created.id);
    EXPECT_FALSE(games->createGame("missing", input));
}

TEST_F(GameStorage, SqlRepresentationAndConstraints)
{
    GameInput input;
    input.name = "Game";
    input.platforms = {GamePlatform::PC, GamePlatform::SWITCH,
                       GamePlatform::SWITCH_2, GamePlatform::PS_5,
                       GamePlatform::EMULATOR};
    input.hours = 0;
    input.completion = GameCompletion::NOT_STARTED;
    ASSIGN_OR_FAIL(auto game, games->createGame("alice", input));
    ASSIGN_OR_FAIL(
        auto types,
        (db->eval<std::string, int, std::string, int, std::string, int>(
            "SELECT typeof(status), status, typeof(completion), "
            "completion, typeof(hours), hours FROM GameTracking")));
    EXPECT_EQ(types.front(),
              (std::tuple{"integer", 0, "integer", 0, "integer", 0}));
    EXPECT_FALSE(db->execute("INSERT INTO GamePlatforms VALUES (9999, 0)"));
    EXPECT_FALSE(db->execute("INSERT INTO GamePlatforms VALUES (1, 0)"));
    EXPECT_FALSE(db->execute("INSERT INTO GamePlatforms VALUES (1, 99)"));
    EXPECT_FALSE(db->execute("UPDATE GameTracking SET start_date='2025-01-02',"
                             "end_date='2025-01-01'"));
    EXPECT_FALSE(db->execute("UPDATE GameTracking SET hours=0.5"));
    ASSERT_TRUE(*games->deleteGame("alice", game.id));
    ASSIGN_OR_FAIL(auto children,
                   db->eval<int>("SELECT count(*) FROM GamePlatforms"));
    EXPECT_EQ(std::get<0>(children.front()), 0);
}

TEST_F(GameStorage, PlatformFailureRollsBackWholeMutation)
{
    GameInput input;
    input.name = "Original";
    input.platforms = {GamePlatform::PC};
    ASSIGN_OR_FAIL(auto old, games->createGame("alice", input));
    ASSERT_TRUE(db->execute("CREATE TRIGGER FailPlatform BEFORE INSERT ON "
                            "GamePlatforms WHEN NEW.platform=3 BEGIN "
                            "SELECT RAISE(ABORT, 'Injected failure'); END"));
    input.name = "Renamed";
    input.platforms = {GamePlatform::SWITCH, GamePlatform::PS_5};
    EXPECT_FALSE(games->updateGame("alice", old.id, input));
    EXPECT_FALSE(games->createGame("alice", input));
    ASSIGN_OR_FAIL(auto after, games->getGame("alice", old.id));
    ASSERT_TRUE(after);
    EXPECT_EQ(after->input.name, "Original");
    EXPECT_EQ(after->input.platforms, std::vector{GamePlatform::PC});
    EXPECT_TRUE(db->autocommit());
    ASSIGN_OR_FAIL(auto all, games->listGames("alice"));
    EXPECT_EQ(all.size(), 1);
}

TEST_F(GameStorage, CompetingConnectionsHaveOneWinner)
{
    ASSIGN_OR_FAIL(auto connection, SQLite::connectFile(file.path()));
    GameDataSqlite other(std::move(connection));
    ASSERT_TRUE(other.initializeSchema());
    GameInput input;
    input.name = "Race";
    auto pending = std::async(std::launch::async,
                              [&other, &input]
                              {
                                  return other.createGame("alice", input);
                              });
    auto first = games->createGame("alice", input);
    auto second = pending.get();
    EXPECT_NE(first.has_value(), second.has_value());
    const auto &error = first ? second.error() : first.error();
    EXPECT_NE(error.as<RuntimeError>(), nullptr);
    ASSIGN_OR_FAIL(auto rows, other.listGames("alice"));
    EXPECT_EQ(rows.size(), 1);
}

TEST(CsvReader, QuotingBomCrLfAndPhysicalLocations)
{
    ASSIGN_OR_FAIL(auto rows, readCsv("\xef\xbb\xbfGame,Notes\r\nName,\"line "
                                      "1\nline 2, \"\"quote\"\"\"\r\n"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].fields[1], "line 1\nline 2, \"quote\"");
    EXPECT_EQ(rows[1].line, 2);
    for(const auto &invalid : {"a\"b,c", "\"a\"junk", "\"unclosed", "a\rb"})
    {
        EXPECT_FALSE(readCsv(invalid));
    }
}

TEST_F(GameStorage, ImportValidatesEverythingAndRerunsWithoutOverwrite)
{
    std::ostringstream report;
    const std::string invalid =
        "Game,Status,Hours\nGood,Done,0\nBad,Done,1.5\n";
    EXPECT_EQ(importGamesCsv(invalid, "alice", *games, report), 2);
    EXPECT_TRUE(games->listGames("alice")->empty());
    const std::string csv =
        "Game,Platform,2nd Platform,Status,Completion,Hours,"
        "Start date,End date,Review,Notes\n"
        " Hades ,PC,PS 5,Now Playing,Finished,0,,2024-02-29,Pending,"
        "\"  %code{hello}\nnext line  \"\n"
        "Hades,PC,,Done,,,,,,\n"
        "BioShock,,,Queue,,,,,,\n"
        ",,,,,,,,,\n";
    EXPECT_EQ(importGamesCsv(csv, "alice", *games, report), 0);
    EXPECT_THAT(report.str(), HasSubstr("inserted=2"));
    EXPECT_THAT(report.str(), HasSubstr("duplicate-in-file=1"));
    EXPECT_EQ(importGamesCsv(csv, "alice", *games, report), 0);
    EXPECT_THAT(report.str(), HasSubstr("duplicate-in-database=2"));
    ASSIGN_OR_FAIL(auto rows, games->listGames("alice"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[1].input.hours, 0);
    EXPECT_FALSE(rows[1].input.start_date);
    EXPECT_TRUE(rows[1].input.end_date);
    EXPECT_EQ(rows[1].input.platforms.size(), 2);
    EXPECT_EQ(rows[1].input.notes, "%code{hello}\nnext line");
    EXPECT_EQ(
        importGamesCsv("Game,Name,Status\na,b,Done\n", "alice", *games, report),
        2);
}

TEST(GameMarkdown, MacroSpecificHtmlAndConcurrentDocumentState)
{
    ASSIGN_OR_FAIL(auto html, renderGameMarkdown("%code{hello}"));
    EXPECT_THAT(html, HasSubstr("<code>hello</code>"));
    auto future = std::async(std::launch::async, renderGameMarkdown,
                             "%code{independent}");
    EXPECT_TRUE(renderGameMarkdown("**other**"));
    ASSERT_TRUE(future.get());
}

namespace
{
struct TransactionDenials
{
    bool rollback = false;
};

int denyTransaction(void *data, int operation, const char *action, const char *,
                    const char *, const char *)
{
    if(operation != SQLITE_TRANSACTION)
    {
        return SQLITE_OK;
    }
    const auto *denials = static_cast<TransactionDenials *>(data);
    if(std::string_view(action) == "COMMIT" ||
       (denials->rollback && std::string_view(action) == "ROLLBACK"))
    {
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}
} // namespace

TEST_F(GameStorage, CommitFailureRollsBackAndUnrecoverableCleanupRejectsWork)
{
    GameInput input;
    input.name = "Before";
    input.platforms = {GamePlatform::PC};
    ASSIGN_OR_FAIL(auto old, games->createGame("alice", input));
    ASSIGN_OR_FAIL(auto statement, db->statementFromStr("SELECT 1"));
    auto native = sqlite3_db_handle(statement.data());
    TransactionDenials denials;
    ASSERT_EQ(sqlite3_set_authorizer(native, denyTransaction, &denials),
              SQLITE_OK);
    input.name = "After";
    input.platforms = {GamePlatform::SWITCH};
    EXPECT_FALSE(games->updateGame("alice", old.id, input));
    EXPECT_TRUE(db->autocommit());
    ASSIGN_OR_FAIL(auto restored, games->getGame("alice", old.id));
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->input.name, "Before");
    EXPECT_EQ(restored->input.platforms, std::vector{GamePlatform::PC});
    denials.rollback = true;
    EXPECT_FALSE(games->updateGame("alice", old.id, input));
    EXPECT_FALSE(db->autocommit());
    EXPECT_FALSE(games->listGames("alice"));
    EXPECT_FALSE(games->deleteGame("alice", old.id));
    sqlite3_set_authorizer(native, nullptr, nullptr);
    sqlite3_exec(native, "ROLLBACK", nullptr, nullptr, nullptr);
}

TEST_F(GameStorage, CorruptPersistedValuesReturnStorageErrors)
{
    GameInput input;
    input.name = "Game";
    ASSERT_TRUE(games->createGame("alice", input));
    ASSERT_TRUE(db->execute("PRAGMA ignore_check_constraints=ON"));
    ASSERT_TRUE(db->execute("UPDATE GameTracking SET hours=1.5"));
    EXPECT_FALSE(games->listGames("alice"));
    ASSERT_TRUE(db->execute("UPDATE GameTracking SET hours=NULL, status=0.5"));
    EXPECT_FALSE(games->listGames("alice"));
    ASSERT_TRUE(db->execute("UPDATE GameTracking SET status=0, "
                            "start_date='2023-02-29'"));
    EXPECT_FALSE(games->listGames("alice"));
}

TEST_F(GameStorage, ImportReportsPartialWritesAndCanResume)
{
    ASSERT_TRUE(db->execute("CREATE TRIGGER FailGame BEFORE INSERT ON "
                            "GameTracking WHEN NEW.name='Second' BEGIN "
                            "SELECT RAISE(ABORT, 'Injected'); END"));
    const std::string csv =
        "Name,Status\nFirst,Done\nSecond,Done\nThird,Done\n";
    std::ostringstream report;
    EXPECT_EQ(importGamesCsv(csv, "alice", *games, report), 4);
    EXPECT_THAT(report.str(), HasSubstr("1 inserts committed"));
    EXPECT_EQ(games->listGames("alice")->size(), 1);
    ASSERT_TRUE(db->execute("DROP TRIGGER FailGame"));
    EXPECT_EQ(importGamesCsv(csv, "alice", *games, report), 0);
    EXPECT_EQ(games->listGames("alice")->size(), 3);
    EXPECT_THAT(report.str(), HasSubstr("duplicate-in-database=1"));
}

TEST(GameMarkdown, MacroDefinitionsAreDocumentLocalAndOutputIsUnchanged)
{
    ASSIGN_OR_FAIL(
        auto defined,
        renderGameMarkdown("%def{custom}{}{<mark>custom</mark>}%custom{}"));
    EXPECT_THAT(defined, HasSubstr("<mark>custom</mark>"));
    auto other = renderGameMarkdown("%custom{}");
    EXPECT_TRUE(!other ||
                other->find("<mark>custom</mark>") == std::string::npos);
}
