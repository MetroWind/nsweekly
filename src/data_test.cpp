#include <optional>

#include <gtest/gtest.h>

#include "data.hpp"
#include "error.hpp"
#include "utils.hpp"
#include "weekly.hpp"
#include "test_utils.hpp"

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
    ASSIGN_OR_FAIL(auto data, DataSourceSqlite::newFromMemory());
    WeeklyPost p;
    p.format = WeeklyPost::MARKDOWN;
    p.language = "en-US";
    p.raw_content = "aaa";
    p.update_time = secondsToTime(2);
    p.week_begin = secondsToTime(1);
    EXPECT_TRUE(isExpected(data->updateWeekly("mw", std::move(p))));
    ASSIGN_OR_FAIL(std::vector<WeeklyPost> ps,
                   data->getWeeklies("mw", secondsToTime(1), secondsToTime(2)));
    EXPECT_EQ(ps.size(), 1);
    EXPECT_EQ(ps[0].format, p.format);
    EXPECT_EQ(ps[0].raw_content, p.raw_content);
    EXPECT_EQ(ps[0].week_begin, p.week_begin);
    EXPECT_EQ(ps[0].language, p.language);
}
