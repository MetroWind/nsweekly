# The game tracking module

A module to track the users’ video game records, replicating the
functionalities in https://go.mws.rocks/unfair , and adding more
features.

Features: tracking, reviewing, journaling, and screenshot archiving.

This module will be behind the `/games/` URL path.

All markdown text in this module should be processed with MacroDown.

## Tracking

- Replicating the “Tracker” sheet. Each user has their own tracking.
- All tracking cells are public, including to visitors who are not
  logged in. Only the owner can add, edit, or delete their records.
- The tracking is displayed as a table like in the spreadsheet
- The table can be sorted by clicking on the header cells. Sorting
  should be a browser behavior, and is not persisted in the database.
- The table doesn’t need to be editable in-place. The user can click
  an edit button to bring up an edit dialog.
- The user should be able to delete a game.
- The user can click an add-game button to create a record. Name and
  status are required; all other fields are optional.
- Game names are just strings. No auto-completion feature is needed
  for now when the user inputs the name.
- Records are deduplicated by game name within each user's tracking.
  Rigorous name matching is not needed.

### Fields

- Name: the game name.
- Platforms: multiple selections from a hard-coded set of available
  platform choices, with no limit of two selections per game.
- Status: Now Playing, Queue, Shelved, Done, or Wishlist, retaining
  the meanings in the spreadsheet's README.
- Completion: Not Started, Partial, Finished, Platinum, or Endless.
- Hours: time played. An empty value is distinct from zero.
- Start date: the first time the game was played.
- End date: the last time the game was played.
- Notes: notes about the game (markdown).

There is no Review field in this release. Optional fields can be empty,
including either date. Status and completion are independent: a game
can be both Now Playing and Finished.

Resuming a game does not have separate tracking behavior for now.
Day-to-day tracking, such as an “I played it today” button, may be added
later and is outside this release's scope.

### Spreadsheet migration

Provide a temporary command-line option to import an exported Tracker
sheet CSV into a specified user's tracking. In import mode, the server
imports the records and exits without starting the HTTP service.

- Import only the Tracker sheet; reviews are outside this scope.
- Combine the Platform and 2nd Platform columns into the platforms
  list, omitting empty entries.
- Ignore the Review column.
- Preserve empty optional values and the recorded dates and hours.
- Apply the same required-field and per-user game-name deduplication
  rules as normal tracking records.

The command-line option is intended to be removed after migration is
stable.

## Reviewing

Replicate the “Reviews” sheet within Games. Each tracked game can have
one review owned by the same user. Store reviews in a separate table with
a game_id foreign key; deleting a game deletes its review. Scores cover
Story/Lore, Gameplay, Graphics, Audio, and Special, with one optional
overall MacroDown review. Dimensions have no comments. Each dimension
accepts 1–10, including fractions; blank scores permit drafts. Calculate overall in browser
JavaScript as (story + 2 * gameplay + graphics + audio + special) / 6,
only when all dimensions are present. Never store overall in the database.
Store creation/update dates as integer UTC Unix timestamps and display
only yyyy-mm-dd. Public sortable review tables and owner-only native HTML
forms follow tracker behavior. Review creation/editing use a separate
page with a scoring rubric beside the form; review deletion uses a dialog. Reviews CSV migration uses --import-reviews-csv and --import-reviews-user.
Import Tracker first; match existing games by owner and name, preserve
Addition/Update dates as UTC integer timestamps, and skip existing reviews.
Ignore review text and calculated columns. Validate the whole CSV before
inserting.
See designs/design-2-reviews.md.

## Journaling

For each game that a user is playing, the user can write journal
entries in markdown. Design and implementation deferred to future.

## Screenshot archiving

A user can upload screenshot for each games. Design and implementation
deferred to future.
