#pragma once
#include <gmock/gmock.h>
#include "user_data.hpp"
#include "game_data.hpp"
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

// Supplies empty game storage without opening a database in composition tests.
class GameDataMock : public GameDataInterface
{
public:
    // Returns configured public tracker rows.
    MOCK_METHOD(E<std::vector<GameRecord>>, listGames,
        (const std::string&), (override));
    // Retrieves a configured owner-scoped game.
    MOCK_METHOD(E<std::optional<GameRecord>>, getGame,
        (const std::string&, int64_t), (override));
    // Captures validated creation input.
    MOCK_METHOD(E<GameRecord>, createGame,
        (const std::string&, const GameInput&), (override));
    // Captures full field replacement.
    MOCK_METHOD(E<GameRecord>, updateGame,
        (const std::string&, int64_t, const GameInput&), (override));
    // Captures owner-scoped deletion.
    MOCK_METHOD(E<bool>, deleteGame,
        (const std::string&, int64_t), (override));
};
