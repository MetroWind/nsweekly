#include <barrier>
#include <future>
#include <gtest/gtest.h>

#include "user_data.hpp"
#include "test_utils.hpp"

namespace
{
E<int64_t> createConcurrently(UserDataSqlite& users,
                              std::barrier<>& ready, int index)
{
    ready.arrive_and_wait();
    DO_OR_RETURN(users.ensureUser("ensured"));
    return users.createUser(std::format("user {}", index));
}
} // namespace

TEST(UserData, GettingNonExistUserIsNotError)
{
    {
        ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
        auto data = std::make_unique<UserDataSqlite>(std::move(db));
        ASSERT_TRUE(data->initializeSchema());
        ASSIGN_OR_FAIL(auto id, data->getUserID("mw"));
        EXPECT_FALSE(id.has_value());
    }
}

TEST(UserData, DuplicateCreationFailsAndEnsureIsIdempotent)
{
    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    UserDataSqlite users(std::move(db));
    ASSERT_TRUE(users.initializeSchema());
    ASSIGN_OR_FAIL(auto first, users.createUser("mw"));
    EXPECT_FALSE(users.createUser("mw"));
    ASSERT_TRUE(users.ensureUser("mw"));
    ASSIGN_OR_FAIL(auto id, users.getUserID("mw"));
    EXPECT_EQ(id, first);
    ASSIGN_OR_FAIL(auto second, users.createUser("other"));
    EXPECT_NE(first, second);
}

TEST(UserData, ConcurrentCreationReturnsEachRequestsOwnID)
{
    ASSIGN_OR_FAIL(auto db, SQLite::connectMemory());
    UserDataSqlite users(std::move(db));
    ASSERT_TRUE(users.initializeSchema());
    std::barrier ready(8);
    std::vector<std::future<E<int64_t>>> results;
    for(int i = 0; i < 8; ++i)
    {
        results.push_back(std::async(std::launch::async, createConcurrently,
            std::ref(users), std::ref(ready), i));
    }
    for(int i = 0; i < 8; ++i)
    {
        ASSIGN_OR_FAIL(auto inserted, results[i].get());
        ASSIGN_OR_FAIL(auto retrieved,
            users.getUserID(std::format("user {}", i)));
        ASSERT_TRUE(retrieved);
        EXPECT_EQ(inserted, *retrieved);
    }
}
