#include <chrono>

#include <gtest/gtest.h>

#include "utils.hpp"

TEST(Utils, CanCalculateDaysSinceNewYear)
{
    Time t = std::chrono::sys_days(std::chrono::year_month_day(
        std::chrono::year(2000), std::chrono::January, std::chrono::day(3)));
    EXPECT_EQ(daysSinceNewYear(t), 2);
}
