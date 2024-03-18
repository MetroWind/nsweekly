#include <optional>

#include <gtest/gtest.h>

#include "data.hpp"
#include "error.hpp"
#include "utils.hpp"

TEST(DataSource, CanGetUser)
{
    {
        ASSIGN_OR_FAIL(auto data, DataSourceSqlite::newFromMemory());
        ASSIGN_OR_FAIL(auto id, data->getUserID("mw"));
        EXPECT_FALSE(id.has_value());
    }
    {
        ASSIGN_OR_FAIL(auto data, DataSourceSqlite::newFromMemory());
        ASSERT_TRUE(data->createUser("mw").has_value());
        ASSIGN_OR_FAIL(std::optional<int64_t> id, data->getUserID("mw"));
        EXPECT_TRUE(id.has_value());
    }
}
