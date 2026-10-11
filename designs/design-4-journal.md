# Game journal

## Purpose and scope

Add a public, chronological journal to each tracked game. The existing
`/games/:user/:game_id` detail page becomes the journal's reading page.
Its tracking information, notes, and review move into a sidebar. An owner
can create, edit, and delete entries; everyone can read saved entries.
The entry editor includes a compact attachment uploader that reuses the
attachments module.

This design extends [games](design-1-games.md),
[reviews](design-2-reviews.md), and
[attachments](design-3-attachments.md). It describes proposed implementation
work; the feature is not implemented by this document.

The following are requirements from the requested feature:

- A game can have multiple Markdown entries in a linear sequence.
- Management is private to the game owner, including editor pages.
- Saved entries are public to read.
- The game detail page displays the journal with tracking and review in
  a sidebar.
- The editor provides attachment uploads through the attachments module.

The following are proposed defaults that make the first implementation
concrete:

- Oldest entry first, with a link to the latest entry.
- A body and automatic timestamps, without a separate title field.
- Separate create/edit pages and a native deletion confirmation page.
- Immediate publication on save, without drafts or publication scheduling.
- Independent attachment lifetime: removing an entry does not delete files.
- All entries on one detail page initially, without pagination.

Do not add comments, tags, revisions, manual ordering, editable dates,
feeds, journal import/export, or automatic image conversion in this change.
Existing game import/export and review views keep their current scope.

## Current implementation and integration points

`GamesModule` owns game route dispatch and server-rendered templates.
`templates/games/game.html` currently renders tracking and review as two
full-width sections. `GameDataInterface` and `GameDataSqlite` provide
owner-scoped game and review operations in the existing SQLite database.
A journal belongs to a tracked game, not to a global game catalog: two
users tracking the same game have independent journals.

`renderGameMarkdown()` constructs a fresh MacroDown processor for each
call. Reuse it for every entry so MacroDown behavior is consistent with
notes and reviews, and processor state does not pass between entries.
Store source text rather than rendered HTML so editing is lossless and
future renderer changes apply to existing entries.

`AttachmentModule` already streams one multipart file at
`POST /attachments/:user/upload`, checks the session, ownership, and
Origin, and calls `AttachmentService`. Its successful response currently
redirects to the attachment manager. `AttachmentUpload` already contains
an owner record, public URL, and duplicate indication. Add a JSON response
mode to this endpoint for the inline uploader while retaining its native
form behavior.

The browser calls the attachment endpoint directly. Games code does not
make an internal HTTP call or access attachment tables or storage paths.
Because the games server only needs to render the owner's upload URL,
`GamesModule` does not need a new `AttachmentService` dependency and `App`
dependency ownership remains unchanged.

## Reading page and responsive layout

Keep the existing detail URL as the canonical reading URL. There is no
new public journal listing route. Existing tracker and review links to a
game continue to work.

The page header contains the game name and existing tracker/reviews
navigation. Beneath it, use a two-column layout:

```text
Game name                         Tracker / Reviews

Journal                           Game information and review
New entry [owner only]             Tracking facts
Jump to latest                    Notes
                                  Review scores and full review
Entry date · Permalink            Game/review actions [owner only]
Rendered Markdown
Edit / Delete [owner only]

Next entry ...
```

Use a grid with a flexible main column and a sidebar approximately
18–24 rem wide. The journal should receive roughly two thirds of the
available width on a typical desktop screen. Use `minmax(0, 1fr)` and
`min-width: 0` to prevent long content from forcing horizontal page
scrolling. Keep the sidebar in normal document flow; a long review must
remain fully readable without an inner scrolling pane.

Preserve every currently displayed tracking field, the full notes,
review dimensions, overall calculated score, timestamps, and full review
body. Keep the existing `data-review-score` inputs and `ReviewScore`
output compatible with `reviews.js`. Do not truncate review content or
change score calculations as part of this layout change.

On narrow screens, replace the side-by-side arrangement with a native
`details` block labeled “Game information and review” before the journal.
The block is initially collapsed on mobile and open on desktop. A small
media-query listener can set the initial `open` state and update it when
the layout crosses its breakpoint; ordinary clicks must continue to work.
Without JavaScript, leave the block open so all existing information is
visible. Use one copy of the content, avoiding duplicate IDs or divergent
mobile and desktop templates. The breakpoint should be chosen from actual
layout testing, with 56 rem as the initial CSS value.

Represent each entry as an `article` with `id="journal-entry-<id>"`.
Its heading contains a human-readable creation timestamp and a permalink.
Display UTC explicitly, consistent with review timestamps, and include an
ISO 8601 `datetime` value in a `time` element. Show an “Updated” timestamp
when it differs from creation time. Same-second edits may have matching
timestamps; timestamps are display metadata, not revision identifiers.

Render entries oldest first using `(added ASC, id ASC)`. Editing does not
change an entry's position. The last item supplies the “Jump to latest”
anchor. Omit that link for an empty journal. An empty journal says “No
journal entries yet”; owners also see “New entry.” Visitors see no
management controls or editor links.

A permalink is `/games/:user/:game_id#journal-entry-:entry_id`. Game
renaming leaves it intact because the route uses IDs. A deleted entry's
anchor no longer resolves, although the game page remains readable.
Entry IDs must never be reused.

Constrain journal and sidebar images to their column width. Tables and
code blocks may scroll horizontally within their own containers. Keep
headings, focus outlines, and links readable in the existing theme. Do
not introduce Bootstrap.

## Entry editor and deletion UI

Use a dedicated editor page containing the game name, a labeled Markdown
textarea, field errors, the attachment block, “Save entry,” and “Cancel.”
Explain near Save that saving makes the entry public. Creation starts
with an empty body; editing loads the original source, not rendered HTML.

The entry form is `application/x-www-form-urlencoded`. Its only submitted
field is `text`. Keep upload controls outside this form: nested forms are
invalid, and attachment bytes must not become journal form fields. A
small adjacent upload form can sit visually between the textarea and the
entry's action row, with the Save button associated with the entry form
through its `form` attribute if needed.

Validation failures redisplay the submitted body and an error beside the
textarea. Escape source before placing it in HTML, including textarea
content, so text such as `</textarea>` stays editable text. Successful
create/edit requests redirect with 303 to the saved entry's anchor.
Cancel returns to the game page, or the entry anchor when editing.

Deletion has its own owner-only confirmation page identifying the game
and entry date. Show an escaped source excerpt rather than rendering the
entry again. Explain that removing the entry leaves uploaded attachments
in the attachment manager. POST an empty body to confirm; GET must never
mutate data. After deletion, redirect with 303 to the game page's journal
heading. Extend the existing game deletion confirmation to explain that
it removes that game's review and all journal entries, while leaving
attachments available.

There is no autosave or recovery store in the first version. A browser
reload can lose unsaved text. An upload failure must not reload or replace
the editor. A lost connection during Save can leave publication status
uncertain; show a retryable error where possible, and let the owner check
the detail page before retrying a create. Do not claim exactly-once
creation: double submission or an explicit retry may create two entries.
Disable repeated submission in the enhanced UI, but keep ordinary native
forms functional.

## Data model and schema

Add `GameJournalEntries` after the existing games tables during
`GameDataSqlite::initializeSchema()`:

```sql
CREATE TABLE IF NOT EXISTS GameJournalEntries
(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    game_id INTEGER NOT NULL
        REFERENCES GameTracking(id) ON DELETE CASCADE,
    text TEXT NOT NULL CHECK(length(text) > 0),
    added INTEGER NOT NULL
        DEFAULT (CAST(strftime('%s', 'now') AS INTEGER)),
    updated INTEGER NOT NULL
        DEFAULT (CAST(strftime('%s', 'now') AS INTEGER))
);

CREATE INDEX IF NOT EXISTS GameJournalEntriesOrder
    ON GameJournalEntries(game_id, added, id);
```

Ownership is derived through `GameTracking.user_id` and `Users.name`.
Do not duplicate an owner column, attachment IDs, or a sequence number.
The ordering index supports reading one game's journal and locating its
children for deletion. `AUTOINCREMENT` prevents a deleted public entry ID
from later identifying a new entry.

Keep the existing foreign-key enabling and verification at connection
initialization. SQLite requires foreign keys to be enabled per connection;
`ON DELETE CASCADE` removes child entries with their game. See the
[SQLite foreign-key documentation](https://www.sqlite.org/foreignkeys.html).

This is an additive schema migration. Change the current early return
that creates `GameReviews` so initialization continues through journal
table and index creation. Use the existing schema-initialization error
path; startup must fail if either new schema statement fails. Repeated
startup is idempotent, including recovery after only the table statement
succeeded. Existing games need no backfill and initially have empty
journals. Do not read or modify the developer's live database for tests.

Add these source types in `src/games/game_journal.hpp`:

```cpp
// Editable Markdown source for one public journal entry.
struct GameJournalInput
{
    // Original validated source, retained for subsequent editing.
    std::string text;
};

// A journal entry inherits ownership from its tracked game.
struct GameJournalRecord
{
    // Stable entry identity and parent game identity.
    int64_t id, game_id;
    // Source used to render and edit the entry.
    GameJournalInput input;
    // Storage-generated UTC Unix seconds; added never changes on edit.
    int64_t added, updated;
};

// Rejects missing, blank, invalid, or oversized Markdown source.
E<GameJournalInput> validateJournal(const GameFields &fields);
```

Set `JOURNAL_TEXT_LIMIT` to 256 KiB of UTF-8 source. This is a proposed
application limit, separate from the existing 1 MiB encoded games form
body limit. Worst-case percent encoding of 256 KiB remains below 1 MiB.
The attachment service's configured upload limit continues to govern
files independently.

Use `validGameText()` for the existing UTF-8 and NUL-byte validation.
Reject missing `text`, zero-length text, and text consisting only of ASCII
spaces, tabs, CR, and LF. Accept image-only Markdown and preserve leading
spaces, line endings, trailing whitespace, and all other source bytes;
whitespace can carry Markdown meaning. Report validation through
`GameValidationError` keyed by `text`, allowing normal form redisplay.
Validate again at the storage boundary so callers cannot bypass it.

## Storage interface and query semantics

Extend `GameDataInterface`, `GameDataSqlite`, and `GameDataMock` with:

```cpp
// Lists one owned game's entries in chronological order.
virtual E<std::vector<GameJournalRecord>> listJournal(
    const std::string &user, int64_t game_id) = 0;
// Returns only an entry belonging to this owner and this game.
virtual E<std::optional<GameJournalRecord>> getJournalEntry(
    const std::string &user, int64_t game_id, int64_t entry_id) = 0;
// Creates a new entry under an existing owner-scoped game.
virtual E<GameJournalRecord> createJournalEntry(
    const std::string &user, int64_t game_id,
    const GameJournalInput &input) = 0;
// Replaces source and updated time without moving the entry.
virtual E<GameJournalRecord> updateJournalEntry(
    const std::string &user, int64_t game_id, int64_t entry_id,
    const GameJournalInput &input) = 0;
// Removes only the identified owner's entry in the identified game.
virtual E<bool> deleteJournalEntry(
    const std::string &user, int64_t game_id, int64_t entry_id) = 0;
```

Like existing game reads, owner-scoped means filtering by the named
account; it does not require the caller to be that account. Public route
handlers can call these reads. Authentication and write authorization
remain the responsibility of the HTTP layer as well as owner filtering
in SQL.

Reads join entries to `GameTracking` and `Users`, filtering by username
and game ID; single-entry reads additionally filter by entry ID. A missing
or foreign entry returns an empty optional. Listing returns an empty
vector for no matching entries; the HTTP layer separately resolves the
game to distinguish an empty journal from a missing game.

Create with `INSERT ... SELECT` from a game filtered by owner and game ID,
using `RETURNING` to obtain the new record. No selected parent means 404.
Update and delete must include all three identities in their predicates:
owner, game, and entry. Never authorize using only a prior read followed
by an unscoped update or delete. An entry ID belonging to another game of
the same owner must also fail.

Use bound parameters for every supplied value. Mutations use the existing
`write_mutex`. Each mutation can be a single statement, so no additional
multi-statement transaction is needed. Set update time in SQL and leave
creation time untouched. Return all stored fields from create/update;
avoid a separate readback that could race with deletion. Delete returns
false for no matched entry. Map that result to 404 at the HTTP layer.

Concurrent edits use last successful write wins, as with current game and
review editing. There is no revision check in this version. Concurrent
parent deletion either removes an already-saved child through cascading
deletion or leaves the subsequent mutation with no matching parent/entry.
No operation recreates a deleted game implicitly.

## Routes, parsing, and authorization

Let `G` mean `/games/:user/:game_id` below. All username segments must be
encoded using existing URL helpers; parse the encoded request target
before decoding, as the games dispatcher already does.

| Method and path | Behavior |
| --- | --- |
| GET `G` | Public game page with full journal and sidebar |
| GET `G/journal/new` | Owner-only empty entry editor |
| POST `G/journal/new` | Owner-only create; 303 to saved entry |
| GET `G/journal/:entry_id/edit` | Owner-only source editor |
| POST `G/journal/:entry_id/edit` | Owner-only update; 303 to entry |
| GET `G/journal/:entry_id/delete` | Owner-only confirmation |
| POST `G/journal/:entry_id/delete` | Owner-only delete; 303 to journal |

The permanent public link uses a fragment on GET `G`, not a separate
entry endpoint. POST to the public detail page remains unsupported.

Extend `dispatch()` to parse the journal suffix explicitly. Existing
`id_text` identifies the game; add a separate entry ID parameter to the
internal handling path rather than packing two IDs into one field.
Require positive 64-bit decimal IDs with complete parsing and no trailing
characters. Malformed IDs yield 400. Unknown suffixes or extra path
components yield 404. Update public-action classification and game-ID
parsing so every journal management action is recognized and authorized.

For each management request:

1. Validate the session and preserve token-refresh cookies.
2. Return 401 for guests and 403 for another authenticated account,
   matching current games management routes.
3. For POST, require exactly one canonical same-origin `Origin` header,
   using the existing comparison against `config.url_prefix`.
4. Require URL-encoded journal form data and bound its streamed body to
   the existing games 1 MiB limit before parsing.
5. Resolve the game under the route's owner. Return 404 if absent.
6. Resolve an existing entry under both that owner and game when needed.
7. Parse fields, validate source, and run the owner-scoped mutation.
8. Redirect only after successful persistence.

Generalize the games form parser to explicit allowed-field sets or an
enum for form kind; do not add another ambiguous boolean. Journal
create/edit accepts exactly one `text` field. Reject unknown fields,
duplicate fields, malformed percent encoding, and query action data with
400. Deletion accepts no fields. Preserve current strict handling for
tracking and review forms.

Use 415 for the wrong media type, 413 for an encoded body over the
transport limit, and 400 with editor redisplay for validly parsed but
invalid text. Unexpected storage failures return 500 through the existing
generic games error response. Logs should identify the operation without
logging journal bodies or attachment contents. Retain `Cache-Control:
no-store` because page output includes session-dependent owner controls.

## Rendering and template context

Build the journal context only for the detail page, not each row of the
tracker or reviews table. After resolving the game, load its entries and
render each independently with `renderGameMarkdown()`.

Each template row supplies `id`, `anchor`, escaped display timestamps,
ISO timestamps, `permalink`, `edit_url`, `delete_url`, and rendered body.
Provide `journal_new_url`, `journal_latest_url`, and a journal-empty flag
at page level. Escape ordinary strings and URLs with the established
`gameEscape()` convention. Only renderer output belongs in the HTML body
slot.

If a stored entry cannot render, show its escaped source in a `pre` block
for that entry and log a generic rendering error. Continue rendering
other entries and the sidebar, matching the existing review fallback.
The fallback preserves public access and owner editing even when source
uses syntax a changed renderer cannot process.

MacroDown currently preserves macro-generated HTML. This design reuses
that existing trust policy; it does not introduce a different sanitizer
for journals. Do not insert filenames or uploader status through
`innerHTML`, and do not treat raw source as rendered output. Separate
MacroDown processors isolate parsing state, but do not promise unique
IDs in author-generated HTML across different entries on the same page.
Generated wrapper anchors always use the entry ID.

Keep existing game Open Graph information and the canonical detail URL.
Do not place draft editor text, upload filenames, or account attachment
lists into metadata.

## Attachment response contract

Extend the existing upload route with an explicit response mode selected
by `Accept: application/json`. Native forms without that header retain
the current HTML error pages and 303 success redirect. Use one upload
implementation for both modes: authentication, Origin checking,
streaming, filename validation, byte limits, publication, and deduplication
must remain shared.

For JSON-mode uploads, return 200 for both new and duplicate records:

```json
{
    "id": 42,
    "filename": "screenshot.png",
    "mime_type": "image/png",
    "byte_size": 123456,
    "public_url": "/attachment/0123456789abcdef0123456789abcdef/screenshot.png",
    "already_owned": false
}
```

The service's returned `public_url` is authoritative; never reconstruct it
from the filename in browser code. Return the validated basename from the
record and the probed MIME type from its file. Serialize with the existing
JSON library, including 64-bit IDs without hand-built string escaping.
The browser treats IDs as opaque and does not perform arithmetic on them.

JSON-mode failures use the existing HTTP status with a small JSON body:

```json
{
    "error": {
        "code": "upload_too_large",
        "message": "The file exceeds the upload limit."
    }
}
```

Define stable codes for `login_required`, `owner_required`,
`origin_required`, `invalid_upload`, `unsupported_media_type`,
`upload_too_large`, and `upload_failed`. Keep unexpected internal details
out of messages. Negotiate the response mode before authorization so a
401 or 403 is JSON too. Continue setting refresh cookies and `no-store`.
Do not introduce CORS exceptions, return URLs, or journal IDs as upload
fields. The multipart contract remains exactly one part named `file`.

## Attachment block and insertion behavior

With JavaScript, display a file chooser, Upload button, and a compact
status/result area. Show a short note: uploaded files are public and
remain in Attachments even if the entry is never saved. Link to the
owner's existing attachment manager for reuse and deletion.

On Upload:

1. Require a selected file; cancellation is not an empty-file upload.
2. Disable another upload while this request is pending. Keep the text
   area editable, but disable Save until the upload completes so the
   editor cannot disappear halfway through its normal upload flow.
3. Construct `FormData` containing only `file`, and POST to the encoded
   owner upload URL with `Accept: application/json` and same-origin
   credentials. Let the browser supply multipart Content-Type and its
   boundary. See [MDN's FormData guide](https://developer.mozilla.org/en-US/docs/Web/API/XMLHttpRequest_API/Using_FormData_Objects).
4. Parse the response only if it is successful JSON with the expected
   fields. Infrastructure can still return HTML errors; provide a
   generic upload failure instead of throwing an uncaught parse error.
5. On success, add a result row with filename, public link, and “Insert
   into entry.” Indicate duplicates as successful reuse. Clear the file
   chooser so the same file can be selected again.
6. Restore controls. On failure, preserve the source and existing result
   rows, and show a retryable status in an `aria-live` area.

Uploading does not automatically modify Markdown. Insertion is an explicit
action so the owner decides where and how often to use a file. The result
row stays available for repeated insertion, and a successful upload with
no insertion is still a valid attachment.

For MIME types the existing public download handler serves as inline
raster images, insert `![escaped filename](<public_url>)`. Use that
handler's current allowlist (`image/jpeg`, `image/png`, and `image/webp`)
in a documented browser helper and test it against server behavior. Other types, including active document
formats that are downloaded, insert `[escaped filename](<public_url>)`.
Do not infer image safety from the filename extension.

Build the label from the basename, escaping backslashes and Markdown
brackets and replacing line breaks/control whitespace with spaces. Use
an angle-delimited destination around the service URL so parentheses in
a filename do not terminate a link. Ensure the URL has no literal angle
brackets or newlines; the service percent-encodes the filename segment.
Verify these snippets using the actual MacroDown renderer, including
filenames containing brackets, parentheses, Unicode, quotes, and spaces.

Insert at the textarea's current selection when the owner clicks Insert,
using `setRangeText()`, then focus the textarea and place the cursor after
the snippet. Make the snippet a separate block by adding a newline before
and after where adjacent text does not already provide one. Dispatch an
input event so any editor dirty-state behavior observes the change.

Do not send image-variant requests, compress images automatically, or
create separate journal attachment records. CSS scales rendered images
visually; original file byte size is unchanged.

Without JavaScript, journal editing and saving remain native forms. Show
a link to the attachment manager that opens separately with `rel="noopener"`
so the owner can upload, copy the public URL, and return to the editor.
Hide inline controls until their JavaScript initialization succeeds.

## Attachment lifetime and failure boundaries

Markdown contains public URLs, not foreign keys to attachment records.
Consequently, journal save and attachment upload are independent actions,
without a shared transaction:

| Event | Result |
| --- | --- |
| Upload succeeds, entry saves | Source references the published file if inserted |
| Upload succeeds, entry validation fails | File remains; source is redisplayed |
| Upload succeeds, owner cancels | File remains in attachment manager |
| Upload fails | Entry source is unchanged and can still be saved |
| Entry or game is deleted | Attachment records and public bytes are untouched |
| Attachment is deleted in manager | Existing journal links may break under current reclamation rules |

Do not parse all Markdown to infer references, garbage-collect uninserted
uploads, or claim that an entry protects files from attachment deletion.
Existing content deduplication and other owners' references retain their
normal meaning. Public attachment access already matches public journal
reading; owner-only editing does not make uploads private before Save.

If an upload response is lost after publication, retrying the same file
and filename uses the existing owner-local deduplication behavior. If a
game is deleted while its editor is open, a later Save returns 404; any
successful uploads remain ordinary attachments.

## File changes and implementation order

| File | Responsibility |
| --- | --- |
| `src/games/game_journal.hpp/.cpp` | Source types and validation |
| `src/games/game_journal_data.cpp` | Owner/game-scoped journal queries |
| `src/games/game_data.hpp/.cpp` | Interface declarations and additive schema |
| `src/games/games_module.hpp/.cpp` | Route parsing, management handlers, detail context |
| `src/attachment/attachment_module.hpp/.cpp` | JSON response negotiation and shared upload responses |
| `src/storage_mock.hpp` | New storage methods in the test mock |
| `templates/games/game.html` | Journal main column and existing-content sidebar |
| `templates/games/journal_edit.html` | Native entry form and attachment block |
| `templates/games/journal_delete.html` | Entry deletion confirmation |
| `templates/games/games.html` | Extend existing game deletion confirmation |
| `statics/games/games.css` | Responsive detail layout and journal/editor styling |
| `statics/games/journal.js` | Inline upload, insertion, and submission state |
| `statics/games/game_detail.js` | Responsive disclosure initialization |
| `CMakeLists.txt` | Register implementation and meaningful tests |

Implementation should proceed in these increments:

1. Add types, validation, schema, and storage operations. Update the mock
   and cover owner isolation and cascading deletion before adding routes.
2. Extend attachment upload response negotiation and test both response
   modes. Keep native attachment management working throughout.
3. Add journal route parsing and native create/edit/delete forms. Verify
   these without depending on JavaScript uploads.
4. Render the journal on the detail page and move all current information
   into the sidebar. Preserve review scoring hooks.
5. Add the attachment block's JavaScript and responsive disclosure.
6. Run storage, route, attachment, and browser checks; review mobile and
   desktop output with empty and long content.

Use the project's existing libmw, SQLite, Inja, MacroDown, and JSON
facilities. Public C++ types and interface methods need intention comments.
New filenames and symbols follow the repository's supplied coding style.
Build with `cmake --build build -j24` after configuring the appropriate
test build; run `ctest --test-dir build --output-on-failure`. No package
build flags need changing.

## Verification and acceptance criteria

### Validation and database tests

Use temporary databases and the existing storage test support. Verify:

- Valid Unicode and multiline source survives create/read/edit exactly.
- Missing, whitespace-only, invalid UTF-8, embedded NUL bytes,
  and oversized source are rejected; the exact source limit is accepted.
- Multiple entries belong to one game and list by creation time then ID;
  editing an old entry does not move it or change its creation timestamp.
- Entries with equal creation timestamps have deterministic ordering.
- The same owner cannot read or mutate an entry through another game ID.
- Another owner cannot read through an owner-filtered query or mutate a
  foreign game, and no failed create implicitly creates a game or user.
- Entry deletion removes only that entry; game deletion removes its
  journal and review; attachment records remain untouched.
- Schema initialization works for a fresh database, a pre-journal
  database, repeated startup, and a database containing only the new
  table but not its index.
- Deleted entry IDs are not reused and stored timestamps are returned.

### HTTP and rendering tests

Use route tests with mocks plus targeted temporary-storage integration
coverage to verify actual predicates and cascades. Cover:

- Guest public reads succeed and render entries plus tracking and review.
- Owner controls appear only for the owner. Direct guest/foreign editor,
  create, edit, and delete requests return 401/403 before mutation.
- Refreshed sessions retain replacement cookies on success and failure.
- Invalid Origin, content type, query data, duplicate fields, unexpected
  fields, malformed encoding, invalid IDs, and oversized bodies fail
  with the specified statuses and no mutation.
- Missing games, missing entries, and mismatched game/entry pairs yield
  404 after authorization; GET confirmation pages do not mutate data.
- Create/edit redirects include the returned entry ID, delete returns to
  the journal, and escaped usernames produce valid route URLs.
- Submitted invalid text is redisplayed safely, including textarea-end
  sequences; a rendering failure falls back for only the affected entry.
- Multiple Markdown documents render independently; notes and review
  retain their existing behavior and score hooks.

### Attachment and browser checks

Extend attachment endpoint tests to assert JSON fields from the actual
service result, duplicate success, JSON errors before body receipt,
configured size limits, and native-form redirects. Ensure malformed or
extra multipart parts still fail and interrupted staging is cleaned up
by the existing service behavior.

Use a browser fixture following `tests/games_browser.html` or an
appropriate existing harness for interaction checks. Cover successful
upload and insertion, duplicate upload, selection replacement, label
escaping, non-image links, failure responses containing HTML, lost
connections, retry, and text preservation while uploading. Confirm
exactly one file part is sent and Save is restored after failure.

Inspect the detail and editor pages at narrow and desktop widths with:
empty journal, one entry, many entries, long review, wide table, large
image, code block, and a very long link. Verify keyboard operation,
visible focus, live upload status, working entry anchors, and full access
to sidebar content. Disable JavaScript and confirm public reading,
create/edit/delete, review visibility, and the attachment-manager fallback.

The feature is complete when the owner can publish a Markdown entry with
an uploaded image or file, find it at a stable anchor on the game detail
page, edit or delete it, and a signed-out visitor can read it alongside
the existing tracking and review information. Cross-owner management must
fail even when requests are made directly rather than through the UI.

## Operational limits and future extensions

Deploy the binary with the new templates and static files together.
Backups continue to include the SQLite database and attachment directory.
There is no new configuration, background worker, or storage root.
Rolling back to an older binary leaves the additive journal table in
place; the older binary will not display it. Old binaries also cannot
provide JSON upload responses, so a mismatched deployment must show an
upload error without discarding editor text.

Rendering every entry makes the initial implementation and permanent
anchors straightforward, but page cost grows with journal size. This is
an explicit first-version limit, not a promise of bounded reads. If
journals become large, add pagination with a way to resolve each permanent
entry link to its containing page. Do not add pagination that silently
breaks the existing fragment links.

Drafts, revision conflict detection, selectable historical entry dates,
attachment reference tracking, and automatic image variants each require
additional product decisions and are outside this implementation.
