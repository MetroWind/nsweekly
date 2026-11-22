# Music scrobbling

Status — proposed design.

## Objective and scope

Receive listens from Poweramp through Pano Scrobbler, keep them in NSWeekly,
and give the owner a browser view of their listening history. A user should
be able to configure Pano using the information on the Music settings page
and verify that their first completed play reached the server.

The initial delivery covers ListenBrainz v1 token validation, submission,
and basic history reads, plus a server-rendered Music page and device
settings. Use the existing NSWeekly browser login and shared user identity.
The Music feature has no dependencies on Weekly, Games, or Attachments.
Provider sync, metadata enrichment, feedback, and public profiles are
outside the initial scope.

## Application structure

Add `src/music/music_module.{hpp,cpp}` for browser routes and
`src/music/listenbrainz_api.{hpp,cpp}` for the protocol handlers. Both use
`MusicDataInterface`, implemented by `MusicDataSqlite`. Keep protocol JSON
conversion at the HTTP boundary. Share typed data for listen metadata,
completed plays, device state, and history queries. The templates live
under `templates/music` and the stylesheet and optional script under
`statics/music`.

Following [the module design](design-0-modules.md), wire the Music storage
and handlers explicitly in `App`. Give storage its own connection to the
shared SQLite database and initialize its tables after the shared Users
table. Reuse the existing SQLite wrapper, libmw HTTP interfaces, and
`mw::E<>` error propagation. The browser handlers borrow `SessionService`
and shared user storage. Apply refreshed session cookies using the existing
helper. Browser and API handlers call storage directly.

## API base and device setup

Mount the compatibility routes at `/music/api/listenbrainz/1`. For example,
the submission URL on `https://example.org` is
`https://example.org/music/api/listenbrainz/1/submit-listens`. The leading
path is NSWeekly's namespace and `1` is the ListenBrainz API version.
Build the advertised URL from configured `url-prefix`. During the phone
test, verify whether Pano adds the version segment before choosing the
exact copyable value on the settings page. Record the tested Pano version
and the value in the implementation documentation.
[Koito's versioned API setup](https://koito.io/guides/scrobbler/)

A signed-in user creates a named device token. Generate 32 cryptographically
random bytes with an existing crypto facility and encode them as lowercase
hex. Store a SHA-256 digest with the owner's numeric user ID and a unique
device ID. Show the original token once on a private response with
`Cache-Control: no-store`. A lost token can be replaced by issuing a new
one and revoking the old one. Require HTTPS for deployment. Keep token
values out of query strings, redirects, metrics, and logs.

Device requests use `Authorization: Token <token>`. The digest lookup
establishes the owner and device. The returned `user_name` is the NSWeekly
account name from shared user storage. Browser requests derive ownership
from the session. Use owner-scoped storage operations for every read and
mutation, including token revocation. Settings mutations use POST and
server-validated CSRF protection. Check the token's active state within
the write transaction so revocation and in-flight submissions have a
defined order.

## ListenBrainz contract

The routes below are relative to `/music/api/listenbrainz/1`. Use JSON
responses and test exact response shapes against upstream documentation.

| Method and path | Behavior |
| --- | --- |
| `GET /validate-token` | Return HTTP 200 with `code`, `message`, `valid`, and `user_name` for an active token. A supplied invalid or revoked token returns 200 and `valid: false` without `user_name`. A missing token returns 400. Use the header form for Pano. |
| `POST /submit-listens` | Accept `single`, `import`, and `playing_now`. Return 200 and `{"status":"ok"}` after successful processing. Invalid authorization returns 401 and invalid payloads return 400. |
| `GET /user/{user_name}/listens` | Require an active token whose owner matches the named user. Return newest first in `payload.listens`, with `payload.count` equal to the number returned and `payload.user_id` equal to the account name. |
| `GET /user/{user_name}/playing-now` | Apply the same authorization as history. Return the history envelope with zero or one item, a `payload.playing_now` boolean, and no `listened_at` on the item. |

History accepts `count` and at most one of `min_ts` and `max_ts`. Apply
exclusive timestamp bounds and sort by `listened_at DESC, id DESC` so
ties are deterministic. Default to 25 rows and cap requests at 1,000 rows.
Timestamp-only API paging has a boundary when multiple rows share a
timestamp. The browser uses a two-field cursor to traverse such rows.
Return 401 for missing or invalid read credentials and 404 if the URL
names a different account. This owner-only rule is an intentional
difference from the hosted service. Verify Pano supplies the token on
reads before claiming its in-app history works.
[ListenBrainz core endpoints](https://listenbrainz.readthedocs.io/en/latest/users/api/core.html)
· [JSON envelopes](https://listenbrainz.readthedocs.io/en/latest/users/json.html)

Example submission:

```json
{
  "listen_type": "single",
  "payload": [
    {
      "listened_at": 1763769600,
      "track_metadata": {
        "artist_name": "Sample Artist",
        "track_name": "Sample Track",
        "release_name": "Sample Album",
        "additional_info": { "media_player": "Poweramp" }
      }
    }
  ]
}
```

1. Bound the body before JSON parsing. Match the documented upstream
   limits of 10,240,000 bytes per body, 10,240 bytes per item, and 1,000
   items per batch. Set a JSON nesting limit of 32 levels.
   Treat over-limit bodies as 413 and unsupported media types as 415.
   Keep these limits local to the Music API.
2. Parse the object and require an array with exactly one item for
   `single` and `playing_now`, and at least one for `import`. Require
   nonempty artist and track strings and an optional release string.
   Validate field types, including that `additional_info` is an object
   when present. Preserve bounded additional metadata for readback so
   newer clients can add fields. Keep supplied MBIDs as metadata.
3. For completed plays require an integer Unix timestamp in UTC seconds.
   Accept historical timestamps for delayed delivery and imports. Apply
   minimum timestamp of `1033430400`. Reject timestamps over five
   minutes ahead of server time to allow for device clock skew. `playing_now` requires no timestamp.
   The client decides when a play qualifies. Playback duration cannot be
   established from the submission alone.
4. Validate all items before writing. Within one transaction, recheck
   authorization, insert completed plays with a targeted uniqueness
   conflict handler, and update the device's last successful submission
   time. An exact retry succeeds and inserts zero rows. Roll back on any
   other insert or update error. Return a JSON error with a stable
   `code` and `error` message. Use a retryable 503 for transient database
   contention and 500 for unexpected failures. Never expose SQL or
   submitted metadata in errors.
5. For `playing_now`, replace that device's in-memory entry after
   authorization. Record server receipt time and expire the entry after
   five minutes using a monotonic clock. Prune expired entries on access.
   Show the most recently received active entry for the
   owner. A matching `single` from the same device can clear it if the
   event was received before the completed play's request began. An
   import leaves current playback alone. Revoked tokens' entries are
   immediately ineligible. Restart clears this temporary state.

Confirm Pano's login and submission requests, including whether it sends
`return_msid` with `playing_now` or requests additional read endpoints.
The base scope assumes no server-assigned recording IDs or feedback
support. If the tested client requires another feature for onboarding or
delivery, specify its contract from the upstream API and add it to the
compatibility tests. An unsupported optional feature should produce a
clear API error instead of claiming success.
[ListenBrainz submission format and limits](https://listenbrainz.readthedocs.io/en/latest/users/json.html#submission-json)

## SQLite data

| Table | Fields and constraints |
| --- | --- |
| `MusicDevices` | Numeric primary key, owner ID referencing Users, label, unique token digest, creation time, nullable revocation time and last completed submission time. |
| `MusicListens` | Numeric primary key, owner ID referencing Users, originating device ID, `listened_at`, server receipt time, artist, track, optional release, bounded additional metadata JSON. |

Use integer epoch seconds for stored times. Index listens on
`(owner_id, listened_at DESC, id DESC)`. Index devices by owner for
settings. Keep revoked device records so old listens retain their
provenance. Treat the device as the source of the first accepted copy of a
play. Serialize operations that share the Music SQLite connection for the
entire transaction, including reads on that connection. Use bound SQL
parameters and the project's existing migration and database setup
conventions.

Apply a unique key on owner, timestamp, artist, and track. Trim surrounding
whitespace from artist and track when parsing and compare the remaining
strings exactly. Exclude optional release, MBIDs, and device identity
from the key so retries with extra metadata can match. Preserve the first
accepted metadata. Two submissions with different timestamps are distinct
plays. Two legitimate plays with the same metadata in the same second
can collide, and a retry with changed artist or title can create another
row. The wire format supplies no stable event identifier to resolve those
cases. Avoid fuzzy matching, which would make the result harder to predict.

## Browser presentation

| Route | Behavior |
| --- | --- |
| `GET /music` | Render the signed-in owner's recent history and summary. Accept `from`, `to`, and an older-page cursor. |
| `GET /music/settings` | Render the Pano setup URL, device labels, token creation form, revocation forms, and last successful completed submission per device. |
| `POST /music/devices` | Create a device and show its one-time token. Use a short-lived, one-time server-side result with a redirect if the current session infrastructure supports it. Otherwise render the result directly and consume a one-time form nonce to prevent duplicate issuance on reload. |
| `POST /music/devices/{id}/revoke` | Revoke an owner-scoped device and redirect to settings on success. Repeating the request is harmless. |
| `GET /music/now-playing` | Optional session-authenticated endpoint for refreshing the now-playing row. Return the current state with `Cache-Control: no-store`. |

Use server-rendered templates for the first paint, filtering, paging, and
settings. Render history newest-first, grouped by date, with time, track,
artist, and album when available. Use 50 rows per page and select one extra
row to decide whether to render an Older link. Encode the last displayed
timestamp and ID in the cursor. Validate them as integers, apply a strict
tuple comparison, and carry the selected date range across page links.
Submitting new filters resets the cursor.

Use a named IANA time zone configured for the module, with UTC as the
default, and show the zone near the date controls. Interpret inclusive
`YYYY-MM-DD` inputs using local calendar boundaries. Convert the start
and the start of the day after the end to UTC and query a half-open
interval. Derive the next boundary by calendar arithmetic so daylight
saving transitions work. Permit either bound alone, and return a form
error for malformed or reversed ranges. Default to the most recent
30 calendar days, with an explicit all-history link. Generate paging
links with the resolved range so it does not drift at midnight.

Run summary queries over the same owner and date bounds as history,
without the cursor. Show total completed plays and the five most-played
artists and tracks. Group artists by stored artist and tracks by stored
artist and title. Sort ties by display names for stable output. For a new
account, show a link to device settings. For an empty date range, retain
the controls and show an empty result message.

Escape imported metadata in templates. If the now-playing row is enhanced,
poll only while the page is visible and set text through text-safe DOM
APIs. The initial row renders on the server. Token copy buttons may use
the clipboard API and leave the token selectable if copying fails. The
history page and device forms work with JavaScript disabled. Use private
cache policy for authenticated pages and `no-store` for setup and token
responses.

## Delivery and verification

1. Add storage and verify schema initialization on both a fresh database
   and one created by the current app. Test owner isolation, revocation,
   batch rollback, duplicate batches, timestamp ties, and a read after
   reopening the database.
2. Implement token validation and listen submission. Exercise raw HTTP
   requests for each type, revoked and malformed tokens, invalid JSON,
   required metadata, optional fields, timestamp and body limits, and
   batch retries. Verify now-playing expiry and ensure it never affects
   completed counts. Test concurrent submissions and revocation.
3. Build the settings page and basic server-rendered history. Test CSRF,
   one-time token display, escaped metadata, empty states, cursor paging
   across timestamp ties, and date bounds on a daylight saving transition.
   Add owner-authenticated reads and check their envelopes and bounds.
4. On Android, grant Pano notification access, enable Poweramp in Pano,
   and enter the server URL and token using its ListenBrainz-like option.
   Confirm that login succeeds and a Poweramp play appears once in Music,
   with a device activity timestamp. Repeat with a queued offline play,
   then replay an equivalent HTTP submission and restart NSWeekly.
   History must still contain one row per play. Check the Pano history
   screen separately and record any extra routes or fields it requests.
   This phone test remains pending until the module exists.
5. Add the summary queries and verify that counts and rankings follow
   the selected range. Check history and device setup with JavaScript
   disabled. Include the verified client setup and test commands in the
   README. Build using `cmake --build build -j24` and run the project's
   test suite.
