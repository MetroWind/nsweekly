#include <optional>

#include <gtest/gtest.h>

#include "data.hpp"
#include "error.hpp"
#include "utils.hpp"

TEST(DataSource, GettingNonExistUserIsNotError)
{
    {
        ASSIGN_OR_FAIL(auto data, DataSourceSqlite::newFromMemory());
        ASSIGN_OR_FAIL(auto id, data->getUserID("mw"));
        EXPECT_FALSE(id.has_value());
    }
}
