#include <optional>
#include <chrono>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "data.hpp"
#include "error.hpp"
#include "utils.hpp"
#include "weekly.hpp"
#include "test_utils.hpp"

using ::testing::IsEmpty;

TEST(DataSource, GettingNonExistUserIsNotError)
{
    {
        ASSIGN_OR_FAIL(auto data, DataSourceSqlite::newFromMemory());
        ASSIGN_OR_FAIL(auto id, data->getUserID("mw"));
        EXPECT_FALSE(id.has_value());
    }
}

TEST(DataSource, CanCreateAndGetWeekly)
{
    // This is a Tuesday.
    Time begin = std::chrono::sys_days(std::chrono::January / 4 / 2000);
    // This is also a Tuesday.
    Time end = std::chrono::sys_days(std::chrono::January / 18 / 2000);
    Time weekly_time = std::chrono::sys_days(std::chrono::January / 17 / 2000);

    ASSIGN_OR_FAIL(auto data, DataSourceSqlite::newFromMemory());
    WeeklyPost p;
    p.format = WeeklyPost::MARKDOWN;
    p.language = "en-US";
    p.raw_content = "aaa";
    p.update_time = weekly_time;
    p.week_begin = std::move(weekly_time);
    EXPECT_TRUE(isExpected(data->updateWeekly("mw", std::move(p))));
    ASSIGN_OR_FAIL(std::vector<WeeklyPost> ps,
                   data->getWeeklies("mw", begin, end));
    EXPECT_EQ(ps.size(), 2);
    EXPECT_THAT(ps[0].raw_content, IsEmpty());
    EXPECT_EQ(ps[0].author, "mw");

    EXPECT_EQ(ps[1].format, p.format);
    EXPECT_EQ(ps[1].raw_content, p.raw_content);
    EXPECT_EQ(ps[1].week_begin, p.week_begin);
    EXPECT_EQ(ps[1].language, p.language);
    EXPECT_EQ(ps[1].author, "mw");
}
