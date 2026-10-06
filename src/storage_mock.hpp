#pragma once
#include <gmock/gmock.h>
#include "user_data.hpp"
#include "weekly_data.hpp"

// Supplies weekly responses without touching storage in handler tests.
class WeeklyDataMock : public WeeklyDataInterface
{
public:
    // Returns the configured weekly range.
    MOCK_METHOD(E<std::vector<WeeklyPost>>, getWeeklies,
        (const std::string&, const Time&, const Time&), (const, override));
    // Captures the post submitted by a save handler.
    MOCK_METHOD(E<void>, updateWeekly,
        (const std::string&, WeeklyPost&&), (const, override));
};

// Supplies shared-user responses without opening files in composition tests.
class UserDataMock : public UserDataInterface
{
public:
    // Returns a configured ID or lookup failure.
    MOCK_METHOD(E<std::optional<int64_t>>, getUserID,
        (const std::string&), (const, override));
    // Returns a configured newly inserted ID.
    MOCK_METHOD(E<int64_t>, createUser, (const std::string&), (const, override));
    // Supplies an idempotent ensure result.
    MOCK_METHOD(E<void>, ensureUser, (const std::string&), (const, override));
};
