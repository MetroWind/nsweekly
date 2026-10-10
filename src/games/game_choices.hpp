#pragma once
#include <array>
#include <string_view>

// Stable persisted platform identifiers.
enum class GamePlatform
{
    PC = 0,
    SWITCH = 1,
    SWITCH_2 = 2,
    PS_5 = 3,
    EMULATOR = 4
};
// Stable persisted tracking intentions, independent of completion.
enum class GameStatus
{
    NOW_PLAYING = 0,
    QUEUE = 1,
    SHELVED = 2,
    DONE = 3,
    WISHLIST = 4
};
// Stable persisted completion identifiers; absence is separate.
enum class GameCompletion
{
    NOT_STARTED = 0,
    PARTIAL = 1,
    FINISHED = 2,
    PLATINUM = 3,
    ENDLESS = 4
};
// Connects a persisted choice to its form code and display label.
template <typename T> struct GameChoice
{
    // Explicit integer-backed storage identifier.
    T value;
    // Stable form token.
    std::string_view code;
    // Human-readable display and CSV label.
    std::string_view label;
};
// Canonical platform order for forms, persistence, CSV, and display.
inline constexpr std::array PLATFORM_CHOICES{
    GameChoice{GamePlatform::PC, "pc", "PC"},
    GameChoice{GamePlatform::SWITCH, "switch", "Switch"},
    GameChoice{GamePlatform::SWITCH_2, "switch_2", "Switch 2"},
    GameChoice{GamePlatform::PS_5, "ps_5", "PS 5"},
    GameChoice{GamePlatform::EMULATOR, "emulator", "Emulator"}};
// Canonical status order and labels.
inline constexpr std::array STATUS_CHOICES{
    GameChoice{GameStatus::NOW_PLAYING, "now_playing", "Now Playing"},
    GameChoice{GameStatus::QUEUE, "queue", "Queue"},
    GameChoice{GameStatus::SHELVED, "shelved", "Shelved"},
    GameChoice{GameStatus::DONE, "done", "Done"},
    GameChoice{GameStatus::WISHLIST, "wishlist", "Wishlist"}};
// Canonical completion order and labels.
inline constexpr std::array COMPLETION_CHOICES{
    GameChoice{GameCompletion::NOT_STARTED, "not_started", "Not Started"},
    GameChoice{GameCompletion::PARTIAL, "partial", "Partial"},
    GameChoice{GameCompletion::FINISHED, "finished", "Finished"},
    GameChoice{GameCompletion::PLATINUM, "platinum", "Platinum"},
    GameChoice{GameCompletion::ENDLESS, "endless", "Endless"}};
