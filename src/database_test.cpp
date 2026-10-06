#include <gtest/gtest.h>

#include "database.hpp"
#include "error.hpp"
#include "utils.hpp"
#include "test_utils.hpp"

TEST(Database, CanEvaluateAndExecute)
{
    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    ASSERT_TRUE(db->execute("CREATE TABLE test (a INTEGER, b TEXT);")
                .has_value());
    ASSERT_TRUE(db->execute("INSERT INTO test (a, b) VALUES "
                            "(1, \"aaa\"), (2, \"aaa\");")
                .has_value());

    ASSIGN_OR_FAIL(auto result0,
                   (db->eval<int64_t, std::string>("SELECT * FROM test;")));
    EXPECT_EQ(result0.size(), 2);

    ASSIGN_OR_FAIL(auto result1, (db->eval<int64_t, std::string>(
        "SELECT a, b FROM test WHERE a = 1;")));
    EXPECT_EQ(result1.size(), 1);
    EXPECT_EQ(result1[0], (std::make_tuple<int64_t, std::string>(1, "aaa")));

    ASSIGN_OR_FAIL(auto result2, (db->eval<int64_t, std::string>(
        "SELECT a, b FROM test WHERE a = 123;")));
    EXPECT_EQ(result2.size(), 0);

    ASSIGN_OR_FAIL(auto result3,
                   (db->eval<int64_t>("SELECT COUNT(*) FROM test;")));
    EXPECT_EQ(result3.size(), 1);
    EXPECT_EQ(std::get<0>(result3[0]), 2);
}

TEST(Database, ParametrizedStatement)
{
    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    ASSERT_TRUE(db->execute("CREATE TABLE test (a INTEGER, b TEXT);")
                .has_value());
    ASSIGN_OR_FAIL(auto sql, db->statementFromStr(
        "INSERT INTO test (a, b) VALUES (?, ?), (2, ?);"));
    auto result = sql.bind(1, "aaa", "bbb");
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(db->execute(std::move(sql)).has_value());
    ASSIGN_OR_FAIL(auto result0, (db->eval<int64_t, std::string>(
        "SELECT * FROM test;")));
    EXPECT_EQ(result0.size(), 2);
    ASSIGN_OR_FAIL(auto result1, (db->eval<int64_t, std::string>(
        "SELECT * FROM test WHERE b = 'aaa';")));
    EXPECT_EQ(result1.size(), 1);
}

TEST(Database, NullableColumnsAndBusyTimeoutAreConfigured)
{
    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    ASSIGN_OR_FAIL(auto timeout, db->eval<int>("PRAGMA busy_timeout;"));
    ASSERT_EQ(timeout.size(), 1);
    EXPECT_EQ(std::get<0>(timeout[0]), 5000);
    ASSIGN_OR_FAIL(auto columns, (db->eval<std::optional<int64_t>,
        std::optional<std::string>>("SELECT NULL, NULL UNION ALL SELECT 7, 'x';")));
    ASSERT_EQ(columns.size(), 2);
    EXPECT_FALSE(std::get<0>(columns[0]));
    EXPECT_FALSE(std::get<1>(columns[0]));
    EXPECT_EQ(std::get<0>(columns[1]), 7);
    EXPECT_EQ(std::get<1>(columns[1]), "x");
}
