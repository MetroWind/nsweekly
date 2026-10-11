#pragma once
#include "games/game.hpp"

// Maximum UTF-8 source size, independent of the encoded transport limit.
inline constexpr size_t JOURNAL_TEXT_LIMIT = 256 * 1024;

// Editable Markdown source for one public journal entry.
struct GameJournalInput
{
    // Original validated source retained for editing.
    std::string text;
};

// A journal entry inherits ownership from its tracked game.
struct GameJournalRecord
{
    // Stable entry and parent game identities.
    int64_t id, game_id;
    // Original source used for rendering and editing.
    GameJournalInput input;
    // Storage-generated UTC Unix seconds; added never changes on edit.
    int64_t added, updated;
};

// Rejects missing, blank, invalid, or oversized Markdown source.
E<GameJournalInput> validateJournal(const GameFields &fields);
