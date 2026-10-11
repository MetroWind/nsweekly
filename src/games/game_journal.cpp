#include "games/game_journal.hpp"

E<GameJournalInput> validateJournal(const GameFields &fields)
{
    auto found = fields.scalars.find("text");
    if(found == fields.scalars.end() ||
       found->second.find_first_not_of(" \t\r\n") == std::string::npos ||
       found->second.size() > JOURNAL_TEXT_LIMIT ||
       !validGameText(found->second))
    {
        GameValidationError error;
        error.fields["text"] =
            "Enter nonblank UTF-8 Markdown of at most 256 KiB.";
        return std::unexpected(Error(std::move(error)));
    }
    return GameJournalInput{found->second};
}
