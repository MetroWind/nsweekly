#include <barrier>
#include <future>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "weekly_data.hpp"
#include "user_data.hpp"
#include "user_sql.hpp"
#include "storage_test_support.hpp"
#include "test_utils.hpp"

using ::testing::IsEmpty;

namespace
{
E<void> saveConcurrently(WeeklyDataSqlite& weeklies,
                         std::barrier<>& ready, int index)
{
    ready.arrive_and_wait();
    for(int i = 0; i < 10; ++i)
    {
        WeeklyPost post{};
        post.format = WeeklyPost::MARKDOWN;
        post.week_begin = *strToDate("2000-01-03") +
            std::chrono::days(index * 7);
        post.raw_content = std::format("post {}", index);
        DO_OR_RETURN(weeklies.updateWeekly("shared", std::move(post)));
    }
    return {};
}

} // namespace

TEST(WeeklyData, CanCreateAndGetWeekly)
{
    // This is a Tuesday.
    Time begin = std::chrono::sys_days(std::chrono::January / 4 / 2000);
    // This is also a Tuesday.
    Time end = std::chrono::sys_days(std::chrono::January / 18 / 2000);
    Time weekly_time = std::chrono::sys_days(std::chrono::January / 17 / 2000);

    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    ASSERT_TRUE(initializeUsers(*db));
    auto data = std::make_unique<WeeklyDataSqlite>(std::move(db));
    ASSERT_TRUE(data->initializeSchema());
    WeeklyPost p;
    p.format = WeeklyPost::MARKDOWN;
    p.language = "en-US";
    p.raw_content = "aaa";
    p.update_time = weekly_time;
    p.week_begin = std::move(weekly_time);
    const auto expected = p;
    EXPECT_TRUE(isExpected(data->updateWeekly("mw", std::move(p))));
    ASSIGN_OR_FAIL(std::vector<WeeklyPost> ps,
                   data->getWeeklies("mw", begin, end));
    EXPECT_EQ(ps.size(), 2);
    EXPECT_THAT(ps[0].raw_content, IsEmpty());
    EXPECT_EQ(ps[0].author, "mw");

    EXPECT_EQ(ps[1].format, expected.format);
    EXPECT_EQ(ps[1].raw_content, expected.raw_content);
    EXPECT_EQ(ps[1].week_begin, expected.week_begin);
    EXPECT_EQ(ps[1].language, expected.language);
    EXPECT_EQ(ps[1].author, "mw");
}

TEST(WeeklyData, SeparateConnectionsAndReopeningRetainExistingSchema)
{
    TemporaryDatabase file;
    ASSERT_FALSE(file.path().empty());
    {
        ASSIGN_OR_FAIL(auto user_db, SQLite::connectFile(file.path()));
        UserDataSqlite users(std::move(user_db));
        ASSERT_TRUE(users.initializeSchema());
        ASSIGN_OR_FAIL(auto weekly_db, SQLite::connectFile(file.path()));
        WeeklyDataSqlite weeklies(std::move(weekly_db));
        ASSERT_TRUE(weeklies.initializeSchema());
        ASSERT_TRUE(users.ensureUser("mw"));
        auto monday = *strToDate("2000-01-03");
        ASSIGN_OR_FAIL(auto empty, weeklies.getWeeklies(
            "mw", monday, monday + std::chrono::days(8)));
        ASSERT_EQ(empty.size(), 2);
        EXPECT_EQ(empty[0].week_begin, monday);
        EXPECT_EQ(empty[1].week_begin, monday + std::chrono::days(7));
        EXPECT_EQ(empty[0].update_time, Time{});
        WeeklyPost post{};
        post.week_begin = monday;
        post.format = WeeklyPost::MARKDOWN;
        post.language = "en-US";
        post.raw_content = "original";
        ASSERT_TRUE(weeklies.updateWeekly("new", WeeklyPost(post)));
        ASSIGN_OR_FAIL(auto id, users.getUserID("new"));
        ASSERT_TRUE(id);
        post.raw_content = "updated";
        post.language = "zh-CN";
        ASSERT_TRUE(weeklies.updateWeekly("new", std::move(post)));
    }
    ASSIGN_OR_FAIL(auto user_db, SQLite::connectFile(file.path()));
    UserDataSqlite users(std::move(user_db));
    ASSERT_TRUE(users.initializeSchema());
    ASSIGN_OR_FAIL(auto weekly_db, SQLite::connectFile(file.path()));
    WeeklyDataSqlite weeklies(std::move(weekly_db));
    ASSERT_TRUE(weeklies.initializeSchema());
    auto monday = *strToDate("2000-01-03");
    ASSIGN_OR_FAIL(auto posts, weeklies.getWeeklies(
        "new", monday, monday + std::chrono::days(7)));
    ASSERT_EQ(posts.size(), 1);
    EXPECT_EQ(posts[0].raw_content, "updated");
    EXPECT_EQ(posts[0].language, "zh-CN");
    EXPECT_EQ(posts[0].format, WeeklyPost::MARKDOWN);
    EXPECT_GT(posts[0].update_time, Time{});
    EXPECT_FALSE(weeklies.getWeeklies("unknown", monday, monday));
    ASSIGN_OR_FAIL(auto empty, weeklies.getWeeklies("new", monday, monday));
    EXPECT_TRUE(empty.empty());
    ASSIGN_OR_FAIL(auto tuesday, weeklies.getWeeklies(
        "new", monday + std::chrono::days(1), monday + std::chrono::days(2)));
    EXPECT_TRUE(tuesday.empty());
}

TEST(WeeklyData, ConcurrentSavesEnsureExactlyOneSharedUser)
{
    TemporaryDatabase file;
    ASSERT_FALSE(file.path().empty());
    ASSIGN_OR_FAIL(auto user_db, SQLite::connectFile(file.path()));
    UserDataSqlite users(std::move(user_db));
    ASSERT_TRUE(users.initializeSchema());
    ASSIGN_OR_FAIL(auto first_db, SQLite::connectFile(file.path()));
    WeeklyDataSqlite first(std::move(first_db));
    ASSERT_TRUE(first.initializeSchema());
    ASSIGN_OR_FAIL(auto second_db, SQLite::connectFile(file.path()));
    WeeklyDataSqlite second(std::move(second_db));
    std::barrier ready(4);
    std::vector<std::future<E<void>>> results;
    for(int i = 0; i < 4; ++i)
    {
        results.push_back(std::async(std::launch::async, saveConcurrently,
            std::ref(i < 2 ? first : second), std::ref(ready), i));
    }
    for(auto& result: results)
    {
        ASSERT_TRUE(result.get());
    }
    auto monday = *strToDate("2000-01-03");
    ASSIGN_OR_FAIL(auto posts, first.getWeeklies(
        "shared", monday, monday + std::chrono::days(28)));
    ASSERT_EQ(posts.size(), 4);
    for(int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(posts[i].raw_content, std::format("post {}", i));
    }
    ASSIGN_OR_FAIL(auto id, users.getUserID("shared"));
    EXPECT_TRUE(id);
}

TEST(WeeklyData, OpensLegacySchemaWithoutRewritingPosts)
{
    TemporaryDatabase file;
    ASSERT_FALSE(file.path().empty());
    {
        ASSIGN_OR_FAIL(auto legacy, SQLite::connectFile(file.path()));
        ASSERT_TRUE(legacy->execute(
            "CREATE TABLE Users (id INTEGER PRIMARY KEY ASC, name TEXT UNIQUE);"));
        ASSERT_TRUE(legacy->execute(
            "CREATE TABLE Weeklies "
            "(user_id INTEGER REFERENCES Users (id) ON DELETE CASCADE,"
            " week_start INTEGER, update_time INTEGER, format INTEGER,"
            " lang TEXT, content TEXT, UNIQUE (user_id, week_start));"));
        ASSERT_TRUE(legacy->execute("INSERT INTO Users VALUES (42, 'legacy');"));
        ASSERT_TRUE(legacy->execute(
            "INSERT INTO Weeklies VALUES "
            "(42, 946857600, 946944000, 0, 'en-US', '**old**');"));
    }
    ASSIGN_OR_FAIL(auto user_db, SQLite::connectFile(file.path()));
    UserDataSqlite users(std::move(user_db));
    ASSERT_TRUE(users.initializeSchema());
    ASSIGN_OR_FAIL(auto weekly_db, SQLite::connectFile(file.path()));
    WeeklyDataSqlite weeklies(std::move(weekly_db));
    ASSERT_TRUE(weeklies.initializeSchema());
    auto monday = *strToDate("2000-01-03");
    ASSIGN_OR_FAIL(auto posts, weeklies.getWeeklies(
        "legacy", monday, monday + std::chrono::days(1)));
    ASSERT_EQ(posts.size(), 1);
    EXPECT_EQ(posts[0].raw_content, "**old**");
    EXPECT_EQ(posts[0].update_time, *strToDate("2000-01-04"));
    ASSIGN_OR_FAIL(auto id, users.getUserID("legacy"));
    EXPECT_EQ(id, 42);
}
