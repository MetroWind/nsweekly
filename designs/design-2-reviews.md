# Game reviews

## Source and scope

Extend the existing Games module with public per-user reviews and owner-only
native HTML forms. Reuse its authentication, body limits, Origin checking,
HTML escaping, MacroDown renderer, and arcade stylesheet. Creation/editing
use a dedicated page; deletion retains its confirmation dialog. The
tracker remains the source of game identity, platform choices, hours, and
ownership. A review is optional and does not affect tracking status or
completion.

Per-dimension spreadsheet comments are intentionally omitted, as requested.

The reference is [MetroWind’s Unfair Game Reviews](https://docs.google.com/spreadsheets/d/1BK6kReBhj3xyfv-4wJdDBMUCZeZwdjLU3FxTdDkFL1Y/edit).
README and Reviews!A1:L5 were inspected read-only on 2026-10-08 through the
Google Drive connector. Reviews has five scored dimensions, per-cell
comments, an overall formula, addition/update dates, hours looked up from
Tracker, and a calculated text export. This change implements reviewing;
it does not import the Reviews sheet or implement its text-export column.

## Inputs and formula

The canonical dimension order is Story/Lore, Gameplay, Graphics, Audio,
and Special. Each score is optional and accepts finite numbers from 1 to
10 inclusive, including fractions. The spreadsheet's validation permits
fractions. Five means average. Story covers narrative and worldbuilding;
gameplay covers play experience and controls; graphics is judged in the
game's platform/style context; audio covers music, effects, and ambience;
special records the reviewer's subjective attachment.

The browser computes `(story + 2 * gameplay + graphics + audio + special)
/ 6`. Gameplay has double weight. There is no overall score in the database,
HTTP form payload, C++ model, or server-rendered data attributes. The browser
populates that cell before initializing sorting. Display one decimal place;
retain the full result in its numeric sort key. The same function updates
the form's live output on input changes. Empty, nonfinite, or out-of-range
inputs produce “Incomplete”, never an average with implicit zeroes. Drafts
may be saved with any subset of scores, including none.

Individual dimensions have no comments. Each game has one optional overall
MacroDown review text field. Empty text means that no prose review is recorded.
Store source text, render through the same games renderer, and fall back
to escaped source on rendering failure. Escape every ordinary text node,
attribute, and source sort key. Reject malformed UTF-8 and embedded NUL.

## Persistence and ownership

`GameReviews.game_id` is an INTEGER primary key and foreign key to
`GameTracking.id ON DELETE CASCADE`. Consequently each tracked game has
zero or one review. No separate user ID or game-name copy is needed: the
parent supplies both. Renaming the tracker game preserves its review.
Deleting the review leaves the parent intact. Deleting the parent cascades
to its review; the game deletion confirmation explicitly states this.

The table stores five nullable REAL score columns and a non-null TEXT
`text` column, defaulting to the empty string. Every score has a CHECK
constraint permitting NULL or a numeric value in [1, 10]. `added` and
`updated` are non-null INTEGER UTC Unix timestamps in seconds. Defaults and
updates use `CAST(strftime('%s', 'now') AS INTEGER)` for compatibility with
the application's SQLite 3.35 minimum. The server formats these timestamps
as UTC `yyyy-mm-dd`; the database retains the full second-resolution time.
An update preserves `added` and refreshes `updated`. Writes in the same
second may have equal timestamps.

Schema initialization creates the new table idempotently after the tracking
table; existing tracking rows and weekly tables are untouched. Every review
query joins GameTracking and Users, or uses an owner-filtered parent
subquery. Upsert uses INSERT SELECT with an owner predicate and RETURNING;
a nonexistent or foreign parent returns 404 without creating anything.
Deletion also filters through the parent. Writes use the existing game
storage mutex and single atomic SQL statements. Validate both HTTP input
and direct storage callers before binding SQL parameters.

`GameReviewInput` contains only the score array and optional overall text.
`GameReviewRecord` adds parent identity and timestamps. `GameDataInterface`
provides `listReviews(user)`, `saveReview(user, game_id, input)`, and
`deleteReview(user, game_id)`. Production uses the existing game connection;
there is no additional application storage dependency or connection.

## Routes and interactions

- GET `/games/:user/reviews`: public review table; unknown account is 404.
- GET/POST `/games/:user/:game_id/review/edit`: create or replace the review
  for an existing game. An absent review opens with all five scores set to
  5. Existing review scores, including blanks, are preserved when editing.
- GET/POST `/games/:user/:game_id/review/delete`: confirm or delete an
  existing review; missing parent or review is 404.

Mutation forms and their GET pages require the matching authenticated owner.
Guests receive 401 and other authenticated users receive 403. Keep the
existing session refresh behavior. POST requires exactly one matching
Origin, URL-encoded form data, no query action fields, and at most 1 MiB.
Reject unexpected fields (including `overall`) and duplicate scalar fields.
Deletion accepts an empty body only. Invalid scores/text return 422 with
all field errors and escaped original input on the editor page. Successful
writes use 303 redirects to the review table and game anchor. Streaming
body reception enforces the existing limit before buffering the full body.
The encoded-target dispatcher continues to split segments before decoding
usernames, including usernames containing encoded slashes.

Tracker and Reviews links form a second-level navigation within Games.
The shared top navigation still has only Weeklies and Games. Tracker names
link to the review table's matching anchor. An owner-only review icon in
the hidden name-cell action overlay opens the create/edit form. Review game
names link back to tracker anchors; review owner actions use the same
hover/focus overlay for edit/delete. To create a review, select an existing
game in the tracker so duplicate or untracked identities cannot arise.

Review columns are Game, five dimensions, Overall, Hours, Added, Updated,
and Review. Hours always comes from the current tracked game. Overall
review text wraps; the
wide table scrolls within the viewport. The editor page and deletion dialog
remain usable at narrow widths. Form errors link to their fields and receive focus; the live overall
output uses an accessible announcement region.

The add/edit page includes a scoring rubric beside the form, extracted from
README!B4:B22 in the source spreadsheet. The checked-in
`templates/review_rubric.html` retains each dimension's explanation and
examples, with minor spelling/punctuation cleanup. It also includes the
scale, overall weighting, and a source link. This is static reference text;
opening the editor never makes a Google API request. Dimension inputs use
`aria-details` to associate their matching explanations.

The dedicated `review_edit.html` page uses two columns on desktop and
omits the review table. Its rubric panel is sticky below the top navigation
and independently scrollable, with keyboard focus support.
At widths up to 900px, the rubric stacks beneath the form and scrolls with
the page. Both layouts work without JavaScript. The delete dialog keeps
its compact layout and omits the rubric.

The review table defaults to overall descending. Missing values remain last
in either direction; equal sort keys retain the original name/ID ordering.
A separate one-year `nsweekly-reviews-sort` host-only cookie preserves review
sorting without changing tracker preferences. Restore only valid column
indices/directions with existing header buttons. Shared sorting supports
all eleven review columns, with numeric dimensions, overall and hours,
lexicographic ISO dates, and source-text review sorting.

Without JavaScript, scores, dates, hours, and review text remain
readable and forms still submit. Overall stays “Incomplete”; sorting and
live calculations require JavaScript. No server-side calculation fallback
is introduced.

## Validation

Exercise finite fractional/boundary scores, blanks, invalid numbers and
text; owner isolation in storage and routes; replacement and review-only
deletion; immutable addition timestamp and integer timestamp types; game
rename and deletion cascade; and absence of an overall database column.

Application smoke tests cover public tables, owner forms, HTTP validation,
review rendering, duplicate/unknown fields, integer timestamps, and cascade
through the real server. Browser checks cover the known spreadsheet sample
(10,10,8,10,10 gives 9.7), draft behavior, live updates, raw-score sorting,
cookie restoration, action links, mobile table overflow, and native forms.
Run the existing tracker and weekly regressions as well.
