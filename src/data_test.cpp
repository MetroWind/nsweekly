#include <gtest/gtest.h>

#include "data.hpp"
#include "utils.hpp"

TEST(DataSource, CanGetUserID)
{
    ASSIGN_OR_FAIL(auto data, DataSourceSqlite::newFromMemory());
    ASSIGN_OR_FAIL(auto id, data->getUserID("mw"));
    EXPECT_FALSE(id.has_value());
}
