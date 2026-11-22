#include <atomic>
#include <barrier>
#include <future>
#include <regex>
#include <gmock/gmock.h>

#include "auth/auth_mock.hpp"
#include "music/listenbrainz_api.hpp"
#include "music/music_http.hpp"
#include "music/music_module.hpp"
#include "storage_test_support.hpp"
#include "user_sql.hpp"
#include "test_utils.hpp"

namespace
{
using Json = nlohmann::json;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::NiceMock;
using namespace std::chrono_literals;

constexpr int64_t LISTENED_AT = 1763769600;
const std::string API = LISTENBRAINZ_API_PATH;
const std::string ALICE_TOKEN = "+//7//v/+//7//v/+//7/w";
const std::string BOB_TOKEN(64, 'b');
const httplib::Headers ALICE_AUTH{{"Authorization", "token " + ALICE_TOKEN}};
const httplib::Headers BOB_AUTH{{"Authorization", "Token " + BOB_TOKEN}};
const httplib::Headers ALICE_COOKIE{{"Cookie", "access-token=alice"}};
const httplib::Headers BOB_COOKIE{{"Cookie", "access-token=bob"}};
std::atomic<int64_t> clock_seconds;

MusicClock::time_point testClock() noexcept
{
    return MusicClock::time_point(std::chrono::seconds(clock_seconds.load()));
}

MusicListen play(std::string title, int64_t time = LISTENED_AT,
                 std::string artist = "Artist")
{
    return {time, {std::move(artist), std::move(title), {}, {}}};
}

Json item(int64_t time = LISTENED_AT)
{
    return {{"listened_at", time},
            {"track_metadata", {{"artist_name", "  A & B  "},
                                {"track_name", "<First>"},
                                {"release_name", "Album"},
                                {"additional_info", {{"media_player",
                                                      "Poweramp"},
                                                     {"new_field", {1, 2}},
                                                     {"recording_mbid",
                                                      "supplied"}}}}}};
}

Json payload(std::string_view kind, Json items)
{
    return {{"listen_type", kind}, {"payload", std::move(items)}};
}

std::string capture(const std::string& body, const std::regex& pattern)
{
    std::smatch groups;
    if(std::regex_search(body, groups, pattern))
    {
        return groups[1].str();
    }
    return {};
}

class MusicStorage : public testing::Test
{
protected:
    void SetUp() override
    {
        clock_seconds = 100;
        ASSERT_FALSE(file.path().empty());
        ASSIGN_OR_FAIL(auto user_conn, SQLite::connectFile(file.path()));
        users = std::make_unique<UserDataSqlite>(std::move(user_conn));
        ASSERT_TRUE(users->initializeSchema());
        ASSIGN_OR_FAIL(alice, users->createUser("alice"));
        ASSIGN_OR_FAIL(bob, users->createUser("bob"));
        // Simulate an upgrade over an existing database with shared users.
        ASSIGN_OR_FAIL(auto conn, SQLite::connectFile(file.path()));
        ASSERT_TRUE(conn->execute("PRAGMA busy_timeout=0"));
        music = std::make_unique<MusicDataSqlite>(std::move(conn), testClock);
        ASSERT_TRUE(music->initializeSchema());
        ASSERT_TRUE(music->initializeSchema());

        ASSIGN_OR_FAIL(auto alice_digest, musicDigest(ALICE_TOKEN));
        ASSIGN_OR_FAIL(auto bob_digest, musicDigest(BOB_TOKEN));
        ASSERT_TRUE(music->createDevice(alice, "Alice phone", alice_digest, 1));
        ASSERT_TRUE(music->createDevice(bob, "Bob phone", bob_digest, 2));
        ASSIGN_OR_FAIL(auto alice_identity, music->identity(alice_digest));
        ASSIGN_OR_FAIL(auto bob_identity, music->identity(bob_digest));
        ASSERT_TRUE(alice_identity);
        ASSERT_TRUE(bob_identity);
        alice_device = *alice_identity;
        bob_device = *bob_identity;
    }

    TemporaryDatabase file;
    std::unique_ptr<UserDataSqlite> users;
    std::unique_ptr<MusicDataSqlite> music;
    int64_t alice = 0, bob = 0;
    MusicIdentity alice_device{}, bob_device{};
};

class MusicRoutes : public MusicStorage
{
protected:
    void SetUp() override
    {
        MusicStorage::SetUp();
        if(HasFatalFailure()) { return; }
        ON_CALL(auth, getUser(testing::_))
            .WillByDefault([](const Tokens& tokens) -> E<UserInfo>
            {
                return UserInfo{tokens.access_token, tokens.access_token};
            });
        config.data_dir = NSWEEKLY_SOURCE_DIR;
        config.url_prefix = "https://tracker.example/prefix/";
        config.guest_index_user = "alice";
        auto zone = musicTimeZone("America/Los_Angeles");
        ASSERT_TRUE(zone);
        api = std::make_unique<ListenBrainzApi>(*music, *users);
        browser = std::make_unique<MusicModule>(
            config, *music, *users, sessions, **zone);
        api->registerRoutes(server);
        browser->registerRoutes(server);
        const auto port = server.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        worker = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        client->set_read_timeout(5);
    }

    void TearDown() override
    {
        if(worker.joinable())
        {
            server.stop();
            worker.join();
        }
    }

    httplib::Result send(const Json& body, const std::string& params = "")
    {
        return client->Post(API + "/submit-listens" + params, ALICE_AUTH,
                            body.dump(), "application/json; charset=utf-8");
    }

    std::string formNonce(const std::string& html)
    {
        return capture(html, std::regex("name=\"csrf\" value=\"([a-f0-9]{64})"));
    }

    Configuration config{};
    NiceMock<AuthMock> auth;
    SessionService sessions{auth};
    std::unique_ptr<ListenBrainzApi> api;
    std::unique_ptr<MusicModule> browser;
    httplib::Server server;
    std::thread worker;
    std::unique_ptr<httplib::Client> client;
};
}

TEST(MusicSchema, InitializesOnFreshDatabase)
{
    ASSIGN_OR_FAIL(auto conn, SQLite::connectMemory());
    ASSERT_TRUE(initializeUsers(*conn));
    MusicDataSqlite storage(std::move(conn));
    EXPECT_TRUE(storage.initializeSchema());
    ASSIGN_OR_FAIL(auto rows, storage.history(100, {}));
    EXPECT_TRUE(rows.empty());
}

TEST_F(MusicStorage, RetryKeepsOriginalMetadataAndDeviceAcrossReopen)
{
    auto first = play("First");
    first.track.release = "Original";
    auto retry = first;
    retry.track.release = "Changed";
    ASSERT_TRUE(music->complete(alice_device, {first, play("Second")}, 10, {}));
    ASSERT_TRUE(music->createDevice(alice, "Other", "other-digest", 11));
    ASSIGN_OR_FAIL(auto other, music->identity("other-digest"));
    ASSERT_TRUE(other);
    ASSERT_TRUE(music->complete(*other, {retry, play("Second")}, 12, {}));
    ASSERT_TRUE(music->complete(bob_device, {retry}, 13, {}));

    music.reset();
    ASSIGN_OR_FAIL(auto conn, SQLite::connectFile(file.path()));
    MusicDataSqlite reopened(std::move(conn));
    ASSIGN_OR_FAIL(auto all, reopened.history(alice, {}));
    ASSERT_EQ(all.size(), 2);
    EXPECT_EQ(all[0].track.title, "Second");
    EXPECT_EQ(all[1].track.release, "Original");
    const MusicQuery older{{}, {{all[0].listened_at, all[0].id}}, 1};
    ASSIGN_OR_FAIL(auto page, reopened.history(alice, older));
    ASSERT_EQ(page.size(), 1);
    EXPECT_EQ(page[0].id, all[1].id);
    ASSIGN_OR_FAIL(auto bobs, reopened.history(bob, {}));
    ASSERT_EQ(bobs.size(), 1);
    EXPECT_EQ(bobs[0].track.release, "Changed");

    ASSIGN_OR_FAIL(auto devices, reopened.devices(alice));
    ASSERT_EQ(devices.size(), 2);
    EXPECT_EQ(devices[0].last_submission, 12);
    EXPECT_EQ(devices[1].last_submission, 10);
    ASSIGN_OR_FAIL(auto inspect, SQLite::connectFile(file.path()));
    ASSIGN_OR_FAIL(auto rows, inspect->eval<int64_t>(
        "SELECT device_id FROM MusicListens WHERE release='Original'"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<0>(rows[0]), alice_device.device_id);
}

TEST_F(MusicStorage, FailedBatchRollsBackAndScopedRevocationIsIdempotent)
{
    ASSIGN_OR_FAIL(auto admin, SQLite::connectFile(file.path()));
    ASSERT_TRUE(admin->execute(
        "CREATE TRIGGER FailActivity BEFORE UPDATE ON MusicDevices "
        "BEGIN SELECT RAISE(ABORT, 'Injected activity error'); END"));
    EXPECT_FALSE(music->complete(alice_device, {play("A"), play("B")}, 10, {}));
    ASSIGN_OR_FAIL(auto rows, music->history(alice, {}));
    EXPECT_TRUE(rows.empty());
    ASSIGN_OR_FAIL(auto devices, music->devices(alice));
    ASSERT_EQ(devices.size(), 1);
    EXPECT_FALSE(devices[0].last_submission);

    ASSERT_TRUE(admin->execute("DROP TRIGGER FailActivity"));
    ASSIGN_OR_FAIL(auto found, music->revokeDevice(bob, alice_device.device_id,
                                                   11));
    EXPECT_FALSE(found);
    ASSERT_TRUE(music->complete(alice_device, {play("A"), play("B")}, 12, {}));
    ASSIGN_OR_FAIL(found, music->revokeDevice(alice, alice_device.device_id, 13));
    EXPECT_TRUE(found);
    ASSIGN_OR_FAIL(found, music->revokeDevice(alice, alice_device.device_id, 14));
    EXPECT_TRUE(found);
    EXPECT_FALSE(music->complete(alice_device, {play("C")}, 15, {}));
    EXPECT_FALSE(music->playing(alice_device, play("C").track));
    ASSIGN_OR_FAIL(devices, music->devices(alice));
    EXPECT_EQ(devices[0].revoked, 13);
    EXPECT_EQ(devices[0].last_submission, 12);
}

TEST_F(MusicStorage, ConcurrentRetryAndRevocationAreSerialized)
{
    std::barrier gate(3);
    auto submit = [this, &gate]
    {
        gate.arrive_and_wait();
        return music->complete(alice_device, {play("Race")}, 10, {});
    };
    auto first = std::async(std::launch::async, submit);
    auto second = std::async(std::launch::async, submit);
    gate.arrive_and_wait();
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());

    std::barrier race(2);
    auto pending = std::async(std::launch::async, [this, &race]
    {
        race.arrive_and_wait();
        return music->complete(alice_device, {play("Revocation race")}, 11, {});
    });
    race.arrive_and_wait();
    ASSIGN_OR_FAIL(auto revoked,
                   music->revokeDevice(alice, alice_device.device_id, 12));
    EXPECT_TRUE(revoked);
    auto submitted = pending.get();
    if(!submitted)
    {
        auto error = submitted.error().as<HTTPError>();
        ASSERT_TRUE(error);
        EXPECT_EQ(error->code, 401);
    }
    ASSIGN_OR_FAIL(auto rows, music->history(alice, {}));
    EXPECT_EQ(rows.size(), submitted ? 2u : 1u);
    EXPECT_FALSE(music->complete(alice_device, {play("After")}, 13, {}));
}

TEST_F(MusicStorage, NowPlayingExpiresAndOlderSingleCannotClearNewerState)
{
    ASSERT_TRUE(music->playing(alice_device, play("Live").track));
    ASSERT_TRUE(music->createDevice(alice, "Other", "other-digest", 2));
    ASSIGN_OR_FAIL(auto other, music->identity("other-digest"));
    ASSERT_TRUE(other);
    clock_seconds = 101;
    ASSERT_TRUE(music->playing(*other, play("Newer").track));
    ASSIGN_OR_FAIL(auto latest, music->nowPlaying(alice));
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->title, "Newer");
    ASSIGN_OR_FAIL(latest, music->nowPlaying(bob));
    EXPECT_FALSE(latest);
    ASSIGN_OR_FAIL(auto revoked, music->revokeDevice(alice, other->device_id, 9));
    ASSERT_TRUE(revoked);
    ASSIGN_OR_FAIL(latest, music->nowPlaying(alice));
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->title, "Live");
    clock_seconds = 120;
    ASSERT_TRUE(music->complete(alice_device, {play("Live")}, 10,
                                MusicClock::time_point(99s)));
    ASSERT_TRUE(music->complete(alice_device, {play("Live")}, 11, {}));
    ASSIGN_OR_FAIL(latest, music->nowPlaying(alice));
    EXPECT_TRUE(latest);
    ASSERT_TRUE(music->complete(alice_device, {play("Live")}, 12, testClock()));
    ASSIGN_OR_FAIL(latest, music->nowPlaying(alice));
    EXPECT_FALSE(latest);
    ASSERT_TRUE(music->playing(alice_device, play("Live").track));

    clock_seconds = 419;
    ASSIGN_OR_FAIL(latest, music->nowPlaying(alice));
    EXPECT_TRUE(latest);
    clock_seconds = 420;
    ASSIGN_OR_FAIL(latest, music->nowPlaying(alice));
    EXPECT_FALSE(latest);
    ASSIGN_OR_FAIL(auto counts, music->summary(alice, {}));
    EXPECT_EQ(counts.count, 1);
}

TEST_F(MusicRoutes, PublicListenBrainzHistoryAndNowPlaying)
{
    auto auth_url = API + "/validate-token";
    auto result = client->Get(auth_url);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 400);
    EXPECT_EQ(Json::parse(result->body)["code"], 400);
    result = client->Get(auth_url, httplib::Headers{{"Authorization", "bad"}});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(Json::parse(result->body),
              (Json{{"code", 200}, {"message", "Token invalid."},
                    {"valid", false}}));
    result = client->Get(auth_url, ALICE_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->get_header_value("Content-Type"), "application/json");
    EXPECT_EQ(Json::parse(result->body),
              (Json{{"code", 200}, {"message", "Token valid."}, {"valid", true},
                    {"user_name", "alice"}}));

    const std::string padded_token = "AAAAAAAAAAAAAAAAAAAAAA==";
    ASSIGN_OR_FAIL(auto padded_digest, musicDigest(padded_token));
    ASSERT_TRUE(music->createDevice(bob, "Padded phone", padded_digest, 3));
    const httplib::Headers padded_auth{
        {"Authorization", "Token " + padded_token}};
    result = client->Get(auth_url, padded_auth);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(Json::parse(result->body)["user_name"], "bob");

    const auto history = API + "/user/alice/listens";
    const auto now = API + "/user/alice/playing-now";
    result = client->Get(history);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    result = client->Get(API + "/user/unknown/listens");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 404);
    result = client->Get(now);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(Json::parse(result->body)["payload"]["count"], 0);
    result = client->Get(API + "/user/unknown/playing-now");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 404);

    auto current = item();
    current.erase("listened_at");
    auto playing = payload("playing_now", {current});
    result = send(playing);
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body), (Json{{"status", "ok"}}));
    result = send(playing, "?return_msid=true");
    ASSERT_TRUE(result);
    auto returned = Json::parse(result->body);
    const std::regex uuid("[a-f0-9]{8}-[a-f0-9]{4}-8[a-f0-9]{3}-"
                          "[89ab][a-f0-9]{3}-[a-f0-9]{12}");
    EXPECT_EQ(returned["status"], "ok");
    EXPECT_TRUE(std::regex_match(returned["recording_msid"].get<std::string>(),
                                 uuid));
    result = send(playing, "?return_msid=true");
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body), returned);
    result = client->Get(now, ALICE_AUTH);
    ASSERT_TRUE(result);
    auto data = Json::parse(result->body).at("payload");
    EXPECT_EQ(data["count"], 1);
    EXPECT_EQ(data["user_id"], "alice");
    EXPECT_EQ(data["playing_now"], true);
    EXPECT_EQ(data["listens"][0]["playing_now"], true);
    EXPECT_FALSE(data["listens"][0].contains("listened_at"));
    EXPECT_EQ(data["listens"][0]["track_metadata"]["additional_info"],
              current["track_metadata"]["additional_info"]);
    auto public_now = data;
    public_now["listens"][0]["track_metadata"].erase("additional_info");
    const httplib::Headers anonymous;
    for(const auto* headers : {&BOB_AUTH, &anonymous})
    {
        result = client->Get(now, *headers);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 200);
        EXPECT_EQ(result->get_header_value("Cache-Control"), "no-store");
        EXPECT_EQ(Json::parse(result->body).at("payload"), public_now);
    }
    result = client->Get(API + "/user/bob/playing-now");
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body)["payload"]["count"], 0);
    result = client->Get(history, ALICE_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body),
              (Json{{"payload", {{"count", 0}, {"user_id", "alice"},
                                 {"listens", Json::array()}}}}));

    auto completed = payload("single", {item()});
    result = send(completed);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(Json::parse(result->body), (Json{{"status", "ok"}}));
    auto batch = payload("import", {item(), item(LISTENED_AT + 1)});
    for(int i = 0; i < 2; ++i)
    {
        result = send(batch);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 200);
    }
    result = client->Get(now, ALICE_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body),
              (Json{{"payload", {{"count", 0}, {"user_id", "alice"},
                                 {"playing_now", true},
                                 {"listens", Json::array()}}}}));
    result = client->Get(history + "?min_ts=1763769600", ALICE_AUTH);
    ASSERT_TRUE(result);
    data = Json::parse(result->body).at("payload");
    EXPECT_EQ(data["count"], 1);
    EXPECT_EQ(data["listens"][0]["listened_at"], LISTENED_AT + 1);
    EXPECT_EQ(data["listens"][0]["track_metadata"]["artist_name"], "A & B");
    EXPECT_EQ(data["listens"][0]["track_metadata"]["additional_info"],
              item()["track_metadata"]["additional_info"]);
    result = client->Get(history + "?min_ts=1763769600");
    ASSERT_TRUE(result);
    ASSERT_EQ(result->status, 200);
    auto public_data = Json::parse(result->body).at("payload");
    EXPECT_EQ(public_data["count"], 1);
    EXPECT_EQ(public_data["listens"][0]["listened_at"], LISTENED_AT + 1);
    const Json public_metadata = {{"artist_name", "A & B"},
                                  {"track_name", "<First>"},
                                  {"release_name", "Album"}};
    EXPECT_EQ(public_data["listens"][0]["track_metadata"], public_metadata);
    result = client->Get(history + "?min_ts=1763769600", BOB_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body).at("payload"), public_data);
    result = client->Get(history + "?count=9000&max_ts=1763769601");
    ASSERT_TRUE(result);
    data = Json::parse(result->body).at("payload");
    EXPECT_EQ(data["count"], 1);
    EXPECT_EQ(data["listens"][0]["listened_at"], LISTENED_AT);
    result = client->Get(history + "?count=0", ALICE_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body)["payload"]["count"], 0);
    result = client->Get(history + "?min_ts=-1", ALICE_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(Json::parse(result->body)["payload"]["count"], 2);
    result = client->Get(history + "?min_ts=1&max_ts=2", ALICE_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 400);
    result = client->Get(API + "/user/alice/listen-count", ALICE_AUTH);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 404);
    EXPECT_EQ(Json::parse(result->body)["code"], 404);
}

TEST_F(MusicRoutes, RejectsInvalidSubmissionsAndReportsContention)
{
    const auto url = API + "/submit-listens";
    auto invalid_json = client->Post(url, ALICE_AUTH, "{", "application/json");
    ASSERT_TRUE(invalid_json);
    EXPECT_EQ(invalid_json->status, 400);
    auto unsupported = client->Post(url, ALICE_AUTH, "{}", "text/plain");
    ASSERT_TRUE(unsupported);
    EXPECT_EQ(unsupported->status, 415);
    auto missing = client->Post(url, "{}", "application/json");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 401);

    auto invalid_track = item();
    invalid_track["track_metadata"]["artist_name"] = "  ";
    auto invalid_extra = item();
    invalid_extra["track_metadata"]["additional_info"] = Json::array();
    auto invalid_release = item();
    invalid_release["track_metadata"]["release_name"] = 23;
    auto invalid_time = item();
    invalid_time["listened_at"] = UINT64_MAX;
    auto fractional = item();
    fractional["listened_at"] = 1763769600.25;
    auto large = item();
    large["track_metadata"]["release_name"] = std::string(10240, 'a');
    Json nesting = 1;
    for(int depth = 0; depth < 32; ++depth)
    {
        nesting = Json::array({std::move(nesting)});
    }
    auto deep = item();
    deep["track_metadata"]["additional_info"]["deep"] = std::move(nesting);

    for(const auto& request : {
        payload("import", {item(), invalid_track}),
        payload("import", Json::array()),
        payload("single", {invalid_extra}),
        payload("single", {invalid_release}),
        payload("single", {invalid_time}),
        payload("single", {fractional}),
        payload("single", {large}),
        payload("single", {deep}),
        payload("single", {item(1033430399)}),
        payload("single", {item(musicEpochSeconds() + 600)}),
        payload("single", {item(), item()}),
        payload("playing_now", {item()})})
    {
        auto result = send(request);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, 400) << result->body;
        EXPECT_EQ(Json::parse(result->body)["code"], 400);
    }
    auto raw_size = item().dump();
    raw_size.insert(1, 10240, ' ');
    auto escaped_key = client->Post(
        url, ALICE_AUTH,
        "{\"listen_type\":\"single\",\"p\\u0061yload\":[" + raw_size + "]}",
        "application/json");
    ASSERT_TRUE(escaped_key);
    EXPECT_EQ(escaped_key->status, 400);
    auto many = Json::array();
    for(int i = 0; i < 1001; ++i) { many.push_back(item()); }
    auto too_many = send(payload("import", std::move(many)));
    ASSERT_TRUE(too_many);
    EXPECT_EQ(too_many->status, 400);
    auto huge = client->Post(url, ALICE_AUTH, std::string(10240001, ' '),
                             "application/json");
    ASSERT_TRUE(huge);
    EXPECT_EQ(huge->status, 413);

    const std::string chunk(64 * 1024, ' ');
    const size_t over_limit = 10240001;
    auto chunked = client->Post(url, ALICE_AUTH,
        [&](size_t offset, httplib::DataSink& sink)
        {
            if(offset == over_limit)
            {
                sink.done();
                return true;
            }
            auto size = std::min(chunk.size(), over_limit - offset);
            return sink.write(chunk.data(), size);
        }, "application/json");
    ASSERT_TRUE(chunked);
    EXPECT_EQ(chunked->status, 413);
    EXPECT_EQ(Json::parse(chunked->body)["code"], 413);

    ASSIGN_OR_FAIL(auto rows, music->history(alice, {}));
    EXPECT_TRUE(rows.empty());
    ASSIGN_OR_FAIL(auto devices, music->devices(alice));
    EXPECT_FALSE(devices[0].last_submission);
    ASSIGN_OR_FAIL(auto admin, SQLite::connectFile(file.path()));
    ASSERT_TRUE(admin->execute("BEGIN IMMEDIATE"));
    auto busy = send(payload("single", {item()}));
    ASSERT_TRUE(busy);
    EXPECT_EQ(busy->status, 503);
    EXPECT_EQ(busy->get_header_value("Retry-After"), "1");
    EXPECT_EQ(Json::parse(busy->body)["code"], 503);
    ASSERT_TRUE(admin->execute("ROLLBACK"));
    auto valid = send(payload("single", {item(1033430400)}));
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->status, 200);
}

TEST_F(MusicRoutes, FormsIssueSecretsOnceAndRevokeOnlyOwnersDevices)
{
    auto settings = client->Get("/music/settings", ALICE_COOKIE);
    ASSERT_TRUE(settings);
    ASSERT_EQ(settings->status, 200) << settings->body;
    EXPECT_EQ(settings->get_header_value("Cache-Control"), "no-store");
    EXPECT_THAT(settings->body,
                HasSubstr("https://tracker.example/prefix/music/api/listenbrainz/"));
    auto nonce = formNonce(settings->body);
    ASSERT_FALSE(nonce.empty());
    const auto form = "label=New+%3Cdevice%3E&csrf=" + nonce;
    constexpr auto TYPE = "application/x-www-form-urlencoded";
    auto response = client->Post("/music/devices", ALICE_COOKIE,
                                 "label=Missing", TYPE);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 403);
    response = client->Post("/music/devices", BOB_COOKIE, form, TYPE);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 403);
    response = client->Post("/music/devices", ALICE_COOKIE, form, TYPE);
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200) << response->body;
    EXPECT_EQ(response->get_header_value("Cache-Control"), "no-store");
    EXPECT_THAT(response->body, HasSubstr("New &lt;device&gt;"));
    const auto token = capture(response->body,
        std::regex(R"(id="device-token">([A-Za-z0-9+/]{21}[AQgw])<)"));
    ASSERT_FALSE(token.empty());
    response = client->Post("/music/devices", ALICE_COOKIE, form, TYPE);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 403);
    EXPECT_THAT(response->body, Not(HasSubstr(token)));

    const httplib::Headers auth_token{{"Authorization", "token " + token}};
    auto sent = client->Post(API + "/submit-listens", auth_token,
                              payload("single", {item()}).dump(),
                              "application/json");
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent->status, 200);
    ASSIGN_OR_FAIL(auto digest, musicDigest(token));
    ASSIGN_OR_FAIL(auto identity, music->identity(digest));
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->owner_id, alice);
    ASSIGN_OR_FAIL(auto admin, SQLite::connectFile(file.path()));
    ASSIGN_OR_FAIL(auto stored, admin->eval<std::string>(
        "SELECT token_digest FROM MusicDevices WHERE label LIKE 'New %'"));
    ASSERT_EQ(stored.size(), 1);
    EXPECT_EQ(std::get<0>(stored[0]), digest);

    auto revoke = "/music/devices/" +
                  std::to_string(identity->device_id) + "/revoke";
    settings = client->Get("/music/settings", BOB_COOKIE);
    ASSERT_TRUE(settings);
    response = client->Post(revoke, BOB_COOKIE,
                            "csrf=" + formNonce(settings->body), TYPE);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    settings = client->Get("/music/settings", ALICE_COOKIE);
    ASSERT_TRUE(settings);
    EXPECT_THAT(settings->body, Not(HasSubstr(token)));
    EXPECT_THAT(settings->body, HasSubstr("New &lt;device&gt;"));
    EXPECT_THAT(settings->body, HasSubstr("Last completed submission · 20"));
    nonce = formNonce(settings->body);
    ASSERT_FALSE(nonce.empty());
    for(int i = 0; i < 2; ++i)
    {
        response = client->Post(revoke, ALICE_COOKIE, "csrf=" + nonce, TYPE);
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 303);
        EXPECT_EQ(response->get_header_value("Location"), "/music/settings");
    }
    response = client->Get(API + "/validate-token", auth_token);
    ASSERT_TRUE(response);
    EXPECT_EQ(Json::parse(response->body)["valid"], false);
    response = client->Get(API + "/user/alice/listens", auth_token);
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200);
    auto history = Json::parse(response->body).at("payload");
    EXPECT_EQ(history["count"], 1);
    EXPECT_FALSE(history["listens"][0]["track_metadata"]
                     .contains("additional_info"));
    response = client->Get(API + "/user/alice/playing-now", auth_token);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(Json::parse(response->body)["payload"]["count"], 0);
    sent = client->Post(API + "/submit-listens", auth_token,
                        payload("single", {item()}).dump(), "application/json");
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent->status, 401);
}

TEST_F(MusicRoutes, HistoryUsesLocalCalendarAndStableCursorThroughTies)
{
    using namespace std::chrono;
    // November 2, 2025 is 25 hours long in Los Angeles.
    const auto start = (sys_days(2025y / November / 2) + 7h)
                           .time_since_epoch() / 1s;
    const auto end = start + 25 * 3600;
    std::vector<MusicListen> batch;
    batch.push_back(play("Before range", start - 1));
    for(int i = 0; i < 51; ++i)
    {
        auto artist = i < 48 ? "Band <b>" : "Minor";
        batch.push_back(play(std::format("Tie {:02}", i), start, artist));
    }
    batch.push_back(play("Tie 50", end - 1, "Minor"));
    batch.push_back(play("After range", end));
    ASSERT_TRUE(music->complete(alice_device, batch, end, {}));
    ASSERT_TRUE(music->complete(bob_device, {play("Bob only", start)}, end, {}));
    ASSERT_TRUE(music->playing(alice_device, play("<Live>").track));

    auto result = client->Get("/music?all=1");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_THAT(result->body, HasSubstr("Music · alice"));
    EXPECT_THAT(result->body, HasSubstr("After range"));
    EXPECT_THAT(result->body, Not(HasSubstr("Bob only")));
    EXPECT_THAT(result->body, Not(HasSubstr("Device settings")));
    EXPECT_THAT(result->body, HasSubstr("Now playing"));
    EXPECT_THAT(result->body, HasSubstr("&lt;Live&gt;"));
    result = client->Get("/music/user/bob?all=1");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_THAT(result->body, HasSubstr("Bob only"));
    EXPECT_THAT(result->body, Not(HasSubstr("After range")));
    EXPECT_THAT(result->body, Not(HasSubstr("Now playing")));
    result = client->Get("/music/user/unknown");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 404);
    result = client->Get("/music/settings");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->get_header_value("Location"), "/login");
    result = client->Get("/music", httplib::Headers{
        {"Cookie", "access-token=new-user"}});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200) << result->body;
    EXPECT_THAT(result->body, HasSubstr("Set up a scrobbling device"));
    result = client->Get("/music?from=2025-11-03&to=2025-11-02", ALICE_COOKIE);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 400) << result->body;
    EXPECT_THAT(result->body, HasSubstr("Enter a valid date range"));
    result = client->Get("/music?from=2025-11-02&to=2025-11-02", ALICE_COOKIE);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->status, 200) << result->body;
    EXPECT_EQ(result->get_header_value("Cache-Control"), "private, no-cache");
    EXPECT_THAT(result->body, HasSubstr("America/Los_Angeles"));
    EXPECT_THAT(result->body, HasSubstr("<strong>52</strong> completed plays"));
    EXPECT_THAT(result->body, HasSubstr("Band &lt;b&gt; <span>48</span>"));
    EXPECT_THAT(result->body, HasSubstr("Tie 50 · Minor <span>2</span>"));
    EXPECT_THAT(result->body, HasSubstr("&lt;Live&gt;"));
    EXPECT_THAT(result->body, HasSubstr(">23:59<"));
    EXPECT_THAT(result->body, Not(HasSubstr("Before range")));
    EXPECT_THAT(result->body, Not(HasSubstr("After range")));
    EXPECT_THAT(result->body, Not(HasSubstr("Bob only")));
    EXPECT_THAT(result->body, Not(HasSubstr("<strong>Tie 01</strong>")));
    auto cursor = capture(result->body,
        std::regex("older=([0-9]+\\.[0-9]+)\" rel=\"next\""));
    ASSERT_FALSE(cursor.empty());
    EXPECT_THAT(result->body, HasSubstr("action=\"/music/user/alice\""));
    EXPECT_THAT(result->body,
                HasSubstr("href=\"/music/user/alice?from=2025-11-02"));
    auto older_url = "/music/user/alice?from=2025-11-02&to=2025-11-02&older=";
    result = client->Get(older_url + cursor, BOB_COOKIE);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_THAT(result->body, HasSubstr("<strong>52</strong> completed plays"));
    EXPECT_THAT(result->body, HasSubstr("<strong>Tie 01</strong>"));
    EXPECT_THAT(result->body, HasSubstr("<strong>Tie 00</strong>"));
    EXPECT_THAT(result->body, Not(HasSubstr("Device settings")));
    EXPECT_THAT(result->body, HasSubstr("Now playing"));
    EXPECT_THAT(result->body, HasSubstr("&lt;Live&gt;"));
    EXPECT_THAT(result->body, Not(HasSubstr("<strong>Tie 02</strong>")));
    EXPECT_THAT(result->body, Not(HasSubstr("rel=\"next\"")));
    result = client->Get("/music?to=2025-11-01", ALICE_COOKIE);
    ASSERT_TRUE(result);
    EXPECT_THAT(result->body, HasSubstr("<strong>1</strong> completed play"));
    EXPECT_THAT(result->body, HasSubstr("Before range"));
    result = client->Get("/music?from=2025-11-04", ALICE_COOKIE);
    ASSERT_TRUE(result);
    EXPECT_THAT(result->body, HasSubstr("No plays in this date range."));
    clock_seconds += 300;
    result = client->Get("/music/user/alice?all=1");
    ASSERT_TRUE(result);
    EXPECT_THAT(result->body, Not(HasSubstr("Now playing")));
    auto expired = client->Get(API + "/user/alice/playing-now");
    ASSERT_TRUE(expired);
    EXPECT_EQ(Json::parse(expired->body)["payload"]["count"], 0);
}
