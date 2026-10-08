#pragma once
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "error.hpp"
#include "game_choices.hpp"

// Proposed tracking values with explicit missing optional fields.
struct GameInput
{
    // Stripped UTF-8 game name, unique per owner.
    std::string name;
    // Deduplicated selections in canonical order.
    std::vector<GamePlatform> platforms;
    // User intention, independent of completion.
    GameStatus status = GameStatus::NOW_PLAYING;
    // Missing completion differs from NOT_STARTED.
    std::optional<GameCompletion> completion;
    // Missing hours differs from a recorded zero.
    std::optional<int32_t> hours;
    // First played calendar day, when recorded.
    std::optional<std::chrono::year_month_day> start_date;
    // Last played day; never precedes a recorded start.
    std::optional<std::chrono::year_month_day> end_date;
    // Stripped MacroDown source, never rendered HTML.
    std::optional<std::string> notes;
};
// One owner's persisted tracking row; IDs survive renames.
struct GameRecord
{
    // Positive storage ID that is never reused after deletion.
    int64_t id;
    // Complete validated tracking fields.
    GameInput input;
};
// Unmodified decoded form or CSV values for validation and redisplay.
struct GameFields
{
    // Original scalar text, including invalid values for redisplay.
    std::map<std::string, std::string> scalars;
    // Original repeated checkbox codes.
    std::vector<std::string> platforms;
};
// Carries every field failure to HTML and CSV boundaries.
struct GameValidationError
{
    // Summary exposed through the shared error container.
    std::string msg = "Invalid game fields";
    // One actionable message for every invalid field.
    std::map<std::string, std::string> fields;
};
// Parses text and validates every field, returning all errors together.
E<GameInput> validateGameInput(const GameFields &fields);
// Revalidates typed storage callers and canonicalizes names and platforms.
E<GameInput> validateGameInput(const GameInput &input);
// Converts typed input to the same form representation used by HTTP.
GameFields gameFields(const GameInput &input);
// Formats a calendar date without timezone conversion.
std::string gameDate(std::chrono::year_month_day date);
// Rejects malformed UTF-8 and embedded NUL in text boundaries.
bool validGameText(std::string_view text);
// Escapes ordinary text for HTML text nodes and quoted attributes.
std::string gameEscape(std::string_view text);
