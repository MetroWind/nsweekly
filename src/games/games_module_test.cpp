#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "games/games_module.hpp"
#include "auth/auth_mock.hpp"
#include "storage_test_support.hpp"
#include "test_utils.hpp"

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::Return;

class GamesHandlers : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        config.data_dir = NSWEEKLY_SOURCE_DIR;
        config.url_prefix = "https://tracker.example";
        ASSIGN_OR_FAIL(auto user_db, SQLite::connectFile(file.path()));
        users = std::make_unique<UserDataSqlite>(std::move(user_db));
        ASSERT_TRUE(users->initializeSchema());
        ASSERT_TRUE(users->ensureUser("alice"));
        ASSIGN_OR_FAIL(auto game_db, SQLite::connectFile(file.path()));
        games = std::make_unique<GameDataSqlite>(std::move(game_db));
        ASSERT_TRUE(games->initializeSchema());
        sessions = std::make_unique<SessionService>(auth);
        module =
            std::make_unique<GamesModule>(config, *games, *users, *sessions);
        UserInfo identity;
        identity.name = "alice";
        ON_CALL(auth, getUser(_)).WillByDefault(Return(identity));
    }
    mw::HTTPServer::Request post(std::string body)
    {
        mw::HTTPServer::Request req;
        req.method = "POST";
        req.target = "/games/alice/new";
        req.body = std::move(body);
        req.set_header("Cookie", "access-token=valid");
        req.set_header("Origin", "https://tracker.example");
        req.set_header("Content-Type", "application/x-www-form-urlencoded");
        return req;
    }
    TemporaryDatabase file;
    Configuration config{};
    ::testing::NiceMock<AuthMock> auth;
    std::unique_ptr<UserDataSqlite> users;
    std::unique_ptr<GameDataSqlite> games;
    std::unique_ptr<SessionService> sessions;
    std::unique_ptr<GamesModule> module;
};

TEST_F(GamesHandlers, PublicAllFieldsAndEscapedOwnerForm)
{
    GameInput input;
    input.name = "<Game>";
    input.notes = "%code{hello}\n\n</textarea>";
    input.hours = 0;
    ASSIGN_OR_FAIL(auto game, games->createGame("alice", input));
    mw::HTTPServer::Request req;
    req.method = "GET";
    mw::HTTPServer::Response res;
    module->handle(req, res, "alice");
    EXPECT_EQ(res.status, 200);
    EXPECT_THAT(res.body, HasSubstr("&lt;Game&gt;"));
    EXPECT_THAT(res.body, HasSubstr("<code>hello</code>"));
    EXPECT_THAT(res.body, Not(HasSubstr(">Add game</a>")));
    EXPECT_EQ(res.get_header_value("Cache-Control"), "no-store");
    req.set_header("Cookie", "access-token=valid");
    mw::HTTPServer::Response edit;
    module->handle(req, edit, "alice", "edit", std::to_string(game.id));
    EXPECT_EQ(edit.status, 200);
    EXPECT_THAT(edit.body, HasSubstr("<dialog open"));
    EXPECT_THAT(edit.body, HasSubstr("%code{hello}\n\n&lt;/textarea&gt;"));
    EXPECT_THAT(edit.body, HasSubstr("value=\"&lt;Game&gt;\""));
}

TEST_F(GamesHandlers, TrackerLinksOnlyExistingOwnerScopedReviews)
{
    GameInput input;
    input.name = "Reviewed";
    ASSIGN_OR_FAIL(auto reviewed, games->createGame("alice", input));
    input.name = "Not reviewed";
    ASSIGN_OR_FAIL(auto unreviewed, games->createGame("alice", input));
    GameReviewInput review;
    ASSERT_TRUE(games->saveReview("alice", reviewed.id, review));
    ASSERT_TRUE(users->ensureUser("bob"));
    input.name = "Foreign";
    ASSIGN_OR_FAIL(auto foreign, games->createGame("bob", input));
    ASSERT_TRUE(games->saveReview("bob", foreign.id, review));
    mw::HTTPServer::Request req;
    req.method = "GET";
    mw::HTTPServer::Response res;
    module->handle(req, res, "alice");
    ASSERT_EQ(res.status, 200);
    EXPECT_THAT(res.body, HasSubstr("data-sort=\"7\">Review"));
    EXPECT_THAT(res.body, HasSubstr("href=\"/games/alice/reviews#game-" +
        std::to_string(reviewed.id) + "\""));
    EXPECT_THAT(res.body, Not(HasSubstr("href=\"/games/alice/reviews#game-" +
        std::to_string(unreviewed.id) + "\"")));
    EXPECT_THAT(res.body, Not(HasSubstr("Foreign")));
    EXPECT_THAT(res.body, Not(HasSubstr("No review")));
    ASSERT_TRUE(games->deleteReview("alice", reviewed.id));
    mw::HTTPServer::Response removed;
    module->handle(req, removed, "alice");
    ASSERT_EQ(removed.status, 200);
    EXPECT_THAT(removed.body, Not(HasSubstr("Read review for")));
}

TEST_F(GamesHandlers, NativeCreateValidationAndFullReplacement)
{
    auto req = post("name=Game&status=now_playing&completion=finished&hours=0"
                    "&platforms=pc&platforms=ps_5&platforms=switch");
    mw::HTTPServer::Response created;
    module->handle(req, created, "alice", "new");
    ASSERT_EQ(created.status, 303);
    EXPECT_EQ(created.get_header_value("Location"), "/games/alice");
    ASSIGN_OR_FAIL(auto rows, games->listGames("alice"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].input.platforms.size(), 3);
    auto invalid = post("name=Entered&status=done&hours=1.5&start_date=bad");
    mw::HTTPServer::Response rejected;
    module->handle(invalid, rejected, "alice", "edit",
                   std::to_string(rows[0].id));
    EXPECT_EQ(rejected.status, 422);
    EXPECT_THAT(rejected.body, HasSubstr("entered: 1.5"));
    EXPECT_THAT(rejected.body, HasSubstr("value=\"Entered\""));
    EXPECT_EQ(games->getGame("alice", rows[0].id)->value().input.name, "Game");
    auto clear = post("name=Renamed&status=done&notes=++%0A");
    mw::HTTPServer::Response saved;
    module->handle(clear, saved, "alice", "edit", std::to_string(rows[0].id));
    ASSERT_EQ(saved.status, 303);
    ASSIGN_OR_FAIL(auto edited, games->getGame("alice", rows[0].id));
    ASSERT_TRUE(edited);
    EXPECT_FALSE(edited->input.hours);
    EXPECT_FALSE(edited->input.completion);
    EXPECT_FALSE(edited->input.notes);
    EXPECT_TRUE(edited->input.platforms.empty());
}

TEST_F(GamesHandlers, AuthenticationOriginAndFormStructure)
{
    for(const auto &supplied : {"", "null", "https://other.example"})
    {
        auto req = post("name=Game&status=done");
        req.headers.erase("Origin");
        if(*supplied)
        {
            req.set_header("Origin", supplied);
        }
        mw::HTTPServer::Response res;
        module->handle(req, res, "alice", "new");
        EXPECT_EQ(res.status, 403);
    }
    auto guest = post("name=Game&status=done");
    guest.headers.erase("Cookie");
    mw::HTTPServer::Response unauthorized;
    module->handle(guest, unauthorized, "alice", "new");
    EXPECT_EQ(unauthorized.status, 401);
    auto owner = post("name=Game&status=done");
    mw::HTTPServer::Response forbidden;
    module->handle(owner, forbidden, "bob", "new");
    EXPECT_EQ(forbidden.status, 403);
    for(const auto &body :
        {"name=X&name=X&status=done", "name=X&status=done&owner=alice",
         "name=X&status=done&Review=bad"})
    {
        auto req = post(body);
        mw::HTTPServer::Response res;
        module->handle(req, res, "alice", "new");
        EXPECT_EQ(res.status, 400);
    }
    auto query = post("name=X&status=done");
    query.target += "?name=forged";
    mw::HTTPServer::Response query_res;
    module->handle(query, query_res, "alice", "new");
    EXPECT_EQ(query_res.status, 400);
    auto type = post("name=X&status=done");
    type.headers.erase("Content-Type");
    type.set_header("Content-Type", "application/json");
    mw::HTTPServer::Response type_res;
    module->handle(type, type_res, "alice", "new");
    EXPECT_EQ(type_res.status, 415);
}

TEST_F(GamesHandlers, MissingIdDeleteAndDuplicateErrors)
{
    GameInput input;
    input.name = "Unique";
    ASSIGN_OR_FAIL(auto game, games->createGame("alice", input));
    auto req = post("name=Unique&status=done");
    mw::HTTPServer::Response duplicate;
    module->handle(req, duplicate, "alice", "new");
    EXPECT_EQ(duplicate.status, 500);
    EXPECT_THAT(duplicate.body, Not(HasSubstr("Game name already exists")));
    for(const auto &id : {"0", "-1", "2junk", "9223372036854775808"})
    {
        req.method = "GET";
        mw::HTTPServer::Response res;
        module->handle(req, res, "alice", "edit", id);
        EXPECT_EQ(res.status, 400);
    }
    auto deletion = post("");
    mw::HTTPServer::Response deleted;
    module->handle(deletion, deleted, "alice", "delete",
                   std::to_string(game.id));
    EXPECT_EQ(deleted.status, 303);
    mw::HTTPServer::Response missing;
    module->handle(deletion, missing, "alice", "delete",
                   std::to_string(game.id));
    EXPECT_EQ(missing.status, 404);
}

TEST_F(GamesHandlers, RefreshCookiesSurviveValidationAndPublicProviderFailure)
{
    UserInfo identity;
    identity.name = "alice";
    Tokens replacement;
    replacement.access_token = "new";
    replacement.refresh_token = "new-refresh";
    EXPECT_CALL(auth, refreshTokens("old")).WillOnce(Return(replacement));
    auto req = post("name= &status=bad");
    req.headers.erase("Cookie");
    req.set_header("Cookie", "refresh-token=old");
    mw::HTTPServer::Response res;
    module->handle(req, res, "alice", "new");
    EXPECT_EQ(res.status, 422);
    EXPECT_EQ(res.get_header_value_count("Set-Cookie"), 2);
    EXPECT_CALL(auth, getUser(_))
        .WillOnce(
            Return(std::unexpected(runtimeError("Provider unavailable"))));
    auto read = post("");
    read.method = "GET";
    mw::HTTPServer::Response public_res;
    module->handle(read, public_res, "alice");
    EXPECT_EQ(public_res.status, 200);
}

TEST_F(GamesHandlers, PublicGameDetailsRemainOwnerScoped)
{
    GameInput input;
    input.name = "<Dedicated>";
    input.hours = 42;
    input.notes = "%code{full notes}";
    ASSIGN_OR_FAIL(auto game, games->createGame("alice", input));
    GameReviewInput review;
    review.text = "%code{full review}";
    ASSERT_TRUE(games->saveReview("alice", game.id, review));
    mw::HTTPServer::Request req;
    req.method = "GET";
    mw::HTTPServer::Response res;
    module->handle(req, res, "alice", "view", std::to_string(game.id));
    ASSERT_EQ(res.status, 200);
    EXPECT_THAT(res.body, HasSubstr("&lt;Dedicated&gt;"));
    EXPECT_THAT(res.body, HasSubstr("<code>full notes</code>"));
    EXPECT_THAT(res.body, HasSubstr("<code>full review</code>"));
    EXPECT_THAT(res.body, HasSubstr("42"));
    EXPECT_THAT(res.body, Not(HasSubstr(">Edit game</a>")));
    ASSERT_TRUE(users->ensureUser("bob"));
    mw::HTTPServer::Response foreign;
    module->handle(req, foreign, "bob", "view", std::to_string(game.id));
    EXPECT_EQ(foreign.status, 404);
    mw::HTTPServer::Response invalid;
    module->handle(req, invalid, "alice", "view", "invalid");
    EXPECT_EQ(invalid.status, 400);
    ASSERT_TRUE(games->deleteReview("alice", game.id));
    mw::HTTPServer::Response empty;
    module->handle(req, empty, "alice", "view", std::to_string(game.id));
    EXPECT_EQ(empty.status, 200);
    EXPECT_THAT(empty.body, HasSubstr("No review yet."));
}

TEST_F(GamesHandlers, JournalNativeLifecycleAndPublicReading)
{
    GameInput input;
    input.name = "Journal Game";
    input.notes = "Existing notes";
    ASSIGN_OR_FAIL(auto game, games->createGame("alice", input));
    const auto id = std::to_string(game.id);
    auto req = post("text=First+entry");
    mw::HTTPServer::Response saved;
    module->handle(req, saved, "alice", "journal/new", id);
    ASSERT_EQ(saved.status, 303);
    ASSIGN_OR_FAIL(auto entries, games->listJournal("alice", game.id));
    ASSERT_EQ(entries.size(), 1);
    auto entry_id = std::to_string(entries[0].id);
    EXPECT_EQ(saved.get_header_value("Location"),
        "/games/alice/" + id + "#journal-entry-" + entry_id);
    mw::HTTPServer::Request guest;
    guest.method = "GET";
    mw::HTTPServer::Response reading;
    module->handle(guest, reading, "alice", "view", id);
    ASSERT_EQ(reading.status, 200);
    EXPECT_THAT(reading.body, HasSubstr("First entry"));
    EXPECT_THAT(reading.body, HasSubstr("Existing notes"));
    EXPECT_THAT(reading.body, HasSubstr(" UTC</time>"));
    EXPECT_THAT(reading.body, Not(HasSubstr(">New entry</a>")));
    mw::HTTPServer::Response denied;
    module->handle(guest, denied, "alice", "journal/edit", id, entry_id);
    EXPECT_EQ(denied.status, 401);
    guest.set_header("Cookie", "access-token=valid");
    mw::HTTPServer::Response editor;
    module->handle(guest, editor, "alice", "journal/edit", id, entry_id);
    EXPECT_EQ(editor.status, 200);
    EXPECT_THAT(editor.body, HasSubstr(">First entry</textarea>"));
    req.body = "text=%3C%2Ftextarea%3E%0Asecond";
    mw::HTTPServer::Response edited;
    module->handle(req, edited, "alice", "journal/edit", id, entry_id);
    EXPECT_EQ(edited.status, 303);
    mw::HTTPServer::Response source;
    module->handle(guest, source, "alice", "journal/edit", id, entry_id);
    EXPECT_THAT(source.body, HasSubstr("&lt;/textarea&gt;\nsecond"));
    mw::HTTPServer::Response confirmation;
    module->handle(guest, confirmation, "alice", "journal/delete", id, entry_id);
    EXPECT_EQ(confirmation.status, 200);
    EXPECT_THAT(confirmation.body, HasSubstr("&lt;/textarea&gt;"));
    EXPECT_EQ(games->listJournal("alice", game.id)->size(), 1);
    req.body = "";
    mw::HTTPServer::Response deleted;
    module->handle(req, deleted, "alice", "journal/delete", id, entry_id);
    EXPECT_EQ(deleted.status, 303);
    EXPECT_EQ(deleted.get_header_value("Location"),
        "/games/alice/" + id + "#JournalTitle");
    EXPECT_TRUE(games->listJournal("alice", game.id)->empty());
}

TEST_F(GamesHandlers, JournalRejectsInvalidRequestsWithoutMutation)
{
    GameInput input;
    input.name = "Journal";
    ASSIGN_OR_FAIL(auto game, games->createGame("alice", input));
    const auto id = std::to_string(game.id);
    for(const auto *body : {"text=", "text=+%09%0D%0A", "text=%FF",
        "text=%00", "text=a&text=b", "text=a&name=b", "text=%GG"})
    {
        auto req = post(body);
        mw::HTTPServer::Response res;
        module->handle(req, res, "alice", "journal/new", id);
        EXPECT_EQ(res.status, 400) << body;
        EXPECT_TRUE(games->listJournal("alice", game.id)->empty());
    }
    auto req = post("text=valid");
    req.target += "?text=query";
    mw::HTTPServer::Response query;
    module->handle(req, query, "alice", "journal/new", id);
    EXPECT_EQ(query.status, 400);
    req.target = "/games/alice/1/journal/new";
    for(const auto *bad_id : {"0", "-1", "1x", "9223372036854775808"})
    {
        mw::HTTPServer::Response bad;
        module->handle(req, bad, "alice", "journal/new", bad_id);
        EXPECT_EQ(bad.status, 400);
    }
    mw::HTTPServer::Response missing;
    module->handle(req, missing, "alice", "journal/edit", id, "999");
    EXPECT_EQ(missing.status, 404);
    UserInfo bob;
    bob.name = "bob";
    ON_CALL(auth, getUser(_)).WillByDefault(Return(bob));
    mw::HTTPServer::Response foreign;
    module->handle(req, foreign, "alice", "journal/new", id);
    EXPECT_EQ(foreign.status, 403);
}

TEST_F(GamesHandlers, JournalRenderingIsIsolatedAndKeepsSidebar)
{
    GameInput input;
    input.name = "Isolated journal";
    input.notes = "Notes survive";
    ASSIGN_OR_FAIL(auto game, games->createGame("alice", input));
    ASSIGN_OR_FAIL(auto first, games->createJournalEntry("alice", game.id,
        {"%def{custom}{}{<mark>custom</mark>}%custom{}"}));
    ASSIGN_OR_FAIL(auto broken, games->createJournalEntry("alice", game.id,
        {"%custom{}"}));
    ASSIGN_OR_FAIL(auto last, games->createJournalEntry("alice", game.id,
        {"**Final entry**"}));
    GameReviewInput review;
    review.scores = {1, 2, 3, 4, 5};
    review.text = "Full review remains";
    ASSERT_TRUE(games->saveReview("alice", game.id, review));
    mw::HTTPServer::Request req;
    req.method = "GET";
    mw::HTTPServer::Response res;
    module->handle(req, res, "alice", "view", std::to_string(game.id));
    ASSERT_EQ(res.status, 200);
    EXPECT_THAT(res.body, HasSubstr("<mark>custom</mark>"));
    EXPECT_THAT(res.body, HasSubstr("<p>%custom{}</p>"));
    EXPECT_THAT(res.body, HasSubstr("<strong>Final entry</strong>"));
    EXPECT_THAT(res.body, HasSubstr("Notes survive"));
    EXPECT_THAT(res.body, HasSubstr("Full review remains"));
    EXPECT_THAT(res.body, HasSubstr("data-review-score value=\"2\""));
    EXPECT_THAT(res.body, HasSubstr("id=\"ReviewScore\""));
    EXPECT_LT(res.body.find("id=\"journal-entry-" + std::to_string(first.id)),
        res.body.find("id=\"journal-entry-" + std::to_string(broken.id)));
    EXPECT_LT(res.body.find("id=\"journal-entry-" + std::to_string(broken.id)),
        res.body.find("id=\"journal-entry-" + std::to_string(last.id)));
}
