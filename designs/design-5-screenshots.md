# Game screenshots

Status: proposed design, 2026-10-11. This document specifies implementation
work; it does not implement the feature.

## Requirements and scope

Add a public screenshot gallery for each user's tracked game. A tracked
game belongs to one account; galleries do not combine screenshots from
other accounts tracking the same title.

The owner can upload multiple images and delete screenshots. Everyone,
including guests, can view the gallery. Thumbnails occupy a full-width,
wrapping flex layout. Selecting one opens a popup containing a compressed
preview image and a direct link to the original file. The original
must never be used as a gallery image, popup image, loading placeholder,
responsive image candidate, or error fallback. The direct original link
may open in the browser; forcing a download dialog is unnecessary.

Each successful upload produces two compressed variants on the server.
Each variant has independently configurable codec, quality, and maximum
long-edge size. Supported output settings are `jpg`, `webp`, `avif`, and
`jxl`; accept `jpeg` and `jpegxl` as aliases. Uploading and transforming
files uses the attachment service. Deleting a screenshot also deletes its
attachment record. Deleting a game releases all its screenshot attachments.
Shared physical bytes survive while other attachment references need them.

This extends [games](design-1-games.md) and
[attachments](design-3-attachments.md). Screenshot attachment lifetime
intentionally differs from the independent attachments used in the
[journal](design-4-journal.md).

Proposed initial choices: newest-first ordering, no captions or manual
sorting, one gallery page without pagination, and immediate publication
once both variants exist. Do not add screenshot import/export, an image
editor, a background conversion worker, or automatic regeneration of old
variants in this change.

## Existing components

`AttachmentService::beginUpload()` and `AttachmentWriter` stream files
without buffering the original in memory. `finish()` publishes immutable,
hash-addressed bytes and returns an owner-scoped attachment record.
Repeated uploads with equal owner, bytes, and filename return the existing
record. Different owners can share physical bytes with independent records.

`ensureImageVariant()` validates ownership, converts an image, and caches
its output by source hash and versioned conversion recipe. The current
converter permits JPEG, PNG, and WebP sources and outputs. It already
orients images, preserves aspect ratio, avoids enlargement, strips
metadata from derivatives, and enforces conversion resource limits. Its
color-profile handling needs the corrections specified below.

`deleteAttachment()` serializes mutations, deletes metadata in a SQLite
transaction, and queues unreferenced original/variant bytes for physical
cleanup. Cleanup failures are retried; committed logical deletion remains
successful. Consumers must never unlink attachment files themselves.

`GamesModule` handles public game pages and owner forms. `App` owns the
attachment service, which currently uses a dedicated connection to the
same SQLite database as games. Schema initialization must create game and
attachment tables before creating screenshot tables and triggers.

The attachment HTTP endpoint accepts one multipart file. Preserve that
contract. Batch screenshot uploads use a game-specific endpoint and call
the attachment service directly rather than making internal HTTP requests.

## Configuration and encoders

Add the following settings to `Configuration`, YAML parsing, example
configuration, and README. These are proposed defaults:

```yaml
game-screenshots:
  thumbnail:
    codec: webp
    quality: 75
    long-edge: 320
  preview:
    codec: webp
    quality: 85
    long-edge: 1920
```

Require integral quality in `[1, 100]` and positive integral long-edge
sizes no greater than `attachment_image_dimension`. Require thumbnail
long-edge to be no greater than preview long-edge. Missing fields receive
defaults; explicitly malformed values fail startup with the field name.
For a requested long-edge `L`, pass `max_width=L` and `max_height=L` to
`ImageVariantOptions`. A square containment bound limits either orientation
without cropping or changing aspect ratio.

Normalize codec aliases to a stable C++ enum before storing settings:

| YAML value | Enum / stored integer | Magick coder | URL suffix | MIME type |
| --- | --- | --- | --- | --- |
| jpg, jpeg | JPEG / 0 | JPEG | .jpg | image/jpeg |
| webp | WEBP / 1 | WEBP | .webp | image/webp |
| avif | AVIF / 2 | AVIF | .avif | image/avif |
| jxl, jpegxl | JXL / 3 | JXL | .jxl | image/jxl |

Keep PNG available to existing attachment consumers; the screenshot output
configuration exposes the four requested codecs. Initially retain the
existing JPEG/PNG/WebP source whitelist. Output support does not imply
acceptance of every ImageMagick input format. Unsupported or corrupt source
files fail explicitly instead of publishing an original-only screenshot.

Use `image.magick()` and an explicit coder prefix when writing to the
staging descriptor. Published filenames are extensionless hashes, so
filesystem suffix inference is unsuitable. Extend validation and generated
URL naming together. Quality is encoder-specific and does not promise an
output byte size or equal fidelity across codecs. Preserve the existing
JPEG white-background behavior for transparent sources.

ImageMagick exposes formats according to its installed libraries and
policy. Query writer availability and perform a tiny real encode for each
configured output codec at startup, using disposable staging outside the
published store. Report unavailable encoders or blocked policies clearly;
do not silently substitute another codec. Test encoded signatures and
readability, not only the output filename. Encoder-specific quality behavior
must be verified against the deployed Magick++ build. See the official
[format list](https://imagemagick.org/formats/).

Keep the existing resource and concurrency limits. Each upload requests
the two conversions sequentially through the shared limiter. Browser
support is independent of server encoding support: if a browser cannot
display a chosen variant, show an explanatory error and links to the
compressed file and original. Never substitute the original into an image.

Persist selected codec, long-edge size, and quality with each screenshot. Configuration changes
apply to subsequent uploads; existing galleries continue using their
stored variants. A future explicit migration may regenerate them.

## Color profiles and metadata

Preserve embedded ICC color profiles in thumbnails and previews by default.
Removing a profile does not convert pixel values to sRGB. Pixels encoded
for Display P3, Adobe RGB, or another profile must not be silently treated
as sRGB. Resizing and compression must retain a matching color description.

The current converter calls `image.colorSpace(Magick::sRGBColorspace)` and
then `image.strip()`. Do not carry that sequence into screenshot processing
without correcting its color-management behavior. A colorspace setter is
not sufficient evidence that an arbitrary embedded ICC profile has been
used in a profile-to-profile transform. ImageMagick's strip operation also
removes profiles and color-related chunks; see its official
[strip and profile options](https://imagemagick.org/command-line-options/).

Use this processing order for each derivative:

1. Decode while retaining the source ICC profile and relevant color tags.
2. Apply EXIF orientation and reset the orientation tag to match the
   transformed pixels, preventing a viewer from applying it a second time.
3. Resize and encode while preserving the source color interpretation.
   Any intermediate conversion used by the codec must produce an output
   whose color description matches its decoded pixel values.
4. Retain metadata and profiles supported by the output format, including
   EXIF, XMP, IPTC, comments, and ICC. Do not intentionally strip anything.
   Format conversion may limit which metadata can be represented; do not
   promise byte-identical metadata across codecs. Color information must
   remain sufficient to interpret the encoded pixels correctly.
5. Reopen the encoded output and verify that its profile or equivalent
   signaling is present and consistent. Treat inability to preserve a
   supported source's color interpretation as conversion failure.

If an output path requires sRGB normalization, perform a real ICC transform
from the embedded source profile to a known destination sRGB profile before
removing or replacing the source profile. Embed the destination profile or
verified equivalent signaling in the output. This is an explicit fallback,
not the default: conversion to sRGB can reduce wide-gamut colors. Implement
it through Magick++'s profile transform support and verify that the installed
ImageMagick has working color-management support. Do not silently discard
invalid profiles; return a conversion error when interpretation is unsafe.

For unprofiled inputs, honor supported explicit format color information.
Only assume sRGB when there is no contrary color information. Preserve the
original regardless of derivative success. See ImageMagick's
[color-management documentation](https://imagemagick.org/color-management/).

JPEG alpha compositing must also use a color-consistent working space and
retain matching output signaling; constructing a fresh sRGB background and
copying differently profiled pixels onto it is insufficient.

Add fixtures with embedded sRGB, Display P3, and Adobe RGB profiles, plus
unprofiled, explicitly tagged, and invalid-profile inputs. Verify decoded
colors against a trusted color-managed reference within codec-appropriate
tolerances, in addition to checking profile presence. Confirm that supported metadata is retained, orientation remains correct,
and originals are byte-identical.
Run these checks for each configured output codec, including AVIF and JXL.

## Data model and ownership

Add `GameScreenshots` in the existing database:

| Column | Constraints and meaning |
| --- | --- |
| id | INTEGER primary key, stable screenshot identity |
| game_id | NOT NULL FK to GameTracking(id), ON DELETE CASCADE |
| attachment_id | NOT NULL UNIQUE FK to Attachments(id), ON DELETE CASCADE |
| thumbnail_codec | NOT NULL INTEGER, codec enum value in [0, 3] |
| thumbnail_size | NOT NULL INTEGER CHECK(thumbnail_size > 0), long edge |
| thumbnail_quality | NOT NULL INTEGER CHECK in [1, 100] |
| preview_codec | NOT NULL INTEGER, codec enum value in [0, 3] |
| preview_size | NOT NULL INTEGER CHECK(preview_size > 0), long edge |
| preview_quality | NOT NULL INTEGER CHECK in [1, 100] |
| added | NOT NULL INTEGER, UTC Unix seconds |

Index `(game_id, added DESC, id DESC)`. An attachment record is associated
with at most one screenshot. Another account's attachment can never be
attached to a game. Validate owner equality during association in the
same write transaction and enforce it with an insertion trigger checking
`GameTracking.user_id = Attachments.user_id`. Reject mismatches rather
than relying on controller validation alone. Screenshot associations and
compression settings are immutable after publication.

Define `enum class ScreenshotCodec : int` with explicit values `JPEG = 0`,
`WEBP = 1`, `AVIF = 2`, and `JXL = 3`. These values are persistent database
identifiers: never reorder or reuse them. Validate integers before casting;
an unknown stored value is a data error. Use SQL CHECK constraints for
codec, size, and quality, including `thumbnail_size <= preview_size`.
Configuration limits are validated on insertion; lowering a configured
limit must not invalidate existing screenshot rows.

Do not store serialized recipes in `GameScreenshots`. Each variant is
represented by its three typed columns. Construct `ImageVariantOptions`
from those columns when resolving generated metadata: codec maps to the
attachment format string, size supplies both containment dimensions, and
quality is passed unchanged. The attachment service's existing serialized
cache key remains an internal implementation detail; this change does not
migrate the shared attachment cache schema. Derive that key through the
same helper used by conversion rather than duplicating its string format.
If the shared converter's transformation version changes later, preserve
resolution of existing screenshot outputs in that migration.

Do not duplicate output files or store image BLOBs. Resolve the original's
hash and the reconstructed options through `AttachmentVariants` to obtain
thumbnail and preview metadata. The attachment service retains these mappings while an
original attachment remains. Do not add restrictive output-hash foreign
keys that would prevent the existing variant cleanup algorithm from
running during attachment deletion.

Enable and verify SQLite foreign keys on every connection. SQLite foreign
keys are connection-specific; the mere presence of `REFERENCES` in schema
text is insufficient. See [SQLite foreign keys and cascade actions](
https://www.sqlite.org/foreignkeys.html).

### Durable attachment cleanup

A foreign key cascades from a deleted parent to its children. Deleting a
screenshot child does not delete its attachment parent, and SQL cascades
cannot call the filesystem service. Add a durable queue:

| GameScreenshotCleanup column | Meaning |
| --- | --- |
| attachment_id | Primary key, FK to Attachments(id), ON DELETE CASCADE |
| user_id | NOT NULL FK to Users(id), attachment's owning account |

Install a `BEFORE DELETE` trigger on `GameScreenshots` that inserts its
attachment ID and owner into this queue with `INSERT OR IGNORE ... SELECT`
from `Attachments`. This also runs for screenshots removed by game
cascade. If the attachment itself is being deleted, the queue row either
is not inserted because the parent is absent or is removed by its own
cascade. Verify both paths against SQLite in integration tests.

The queue is also an unpublished-upload ledger: newly created screenshot
attachments enter it atomically with attachment creation. Successful
association removes the queue entry in the same transaction that inserts
the screenshot. Therefore a crash cannot leave an untracked screenshot
attachment between publication and association.

Queue entries contain stable owner IDs, not filenames or public URLs.
They are private operational metadata and are never rendered publicly.

## Services and transaction boundaries

Add `GameScreenshotService`, borrowing `AttachmentService` and the game
storage interface. It coordinates game authorization, variant production,
association, listing, and deletion. `GamesModule` borrows this service.
Keep image encoding and physical reclamation in the attachment module.

Extend attachment upload internals with an explicitly typed screenshot
upload purpose. A screenshot upload's finish transaction registers a new
attachment in `GameScreenshotCleanup` before committing. Ordinary uploads
retain existing behavior. Return whether the record was newly created;
never enqueue an existing attachment merely because it was reuploaded.

Add narrow internal attachment operations for publishing an association
and draining screenshot cleanup. They use the attachment connection under
its existing mutation mutex. Do not expose arbitrary SQL callbacks or
permit games code to manipulate attachment files. Put screenshot-specific
SQL in a dedicated data helper, such as `game_screenshot_data.cpp`, invoked
by these coordinated operations.

Association transaction steps:

1. Acquire the attachment mutation mutex and start a write transaction.
2. Resolve the game and attachment and verify the expected owner.
3. Reject a game that has been deleted, or an attachment already queued
   for deletion other than this operation's own unpublished upload.
4. Verify both requested compression settings and their variant mappings still exist for this source.
5. Insert the screenshot, or return its existing identity for a same-game
   duplicate. Delete this operation's unpublished queue entry on success.
6. Commit before releasing the mutex. Return gallery metadata.

This prevents attachment deletion from racing between verification and
association. Do not hold SQLite transactions or the mutation mutex while
receiving bodies or converting images. Dedicated SQLite connections remain
serialized by SQLite writes; handle busy errors through the existing
storage error policy without holding a game transaction while waiting for
an attachment operation.

The screenshot service serializes its upload, screenshot deletion, game
deletion, and queue-drain coordination within one process. All game-delete
routes must use its deletion entrypoint. Direct attachment manager deletion
still uses the attachment service and its mutex. Concurrent original-only
attachment uploads remain valid and must never be mistaken for abandoned
screenshot uploads.

Drain the queue through `deleteAttachment(user_id, attachment_id)` after
logical deletion commits. Treat an already missing attachment as complete.
A storage failure leaves the queue entry for retry and is logged. Drain at
startup before accepting requests and after screenshot/game mutations.
Do not drain entries belonging to active uploads: hold the coordinator
lock through the upload lifecycle, or explicitly track active IDs and
exclude them under that lock. Choose the lock implementation initially;
it serializes screenshot uploads but leaves ordinary attachments and
public reads independent. Revisit throughput only if measured necessary.

Game deletion is one game transaction: remove `GameTracking`; the cascade
removes screenshot rows and triggers cleanup queue insertion atomically.
Then commit and drain the queue. A crash after commit leaves a durable
queue. A rollback restores the game, screenshots, and queue state together.

Screenshot deletion removes its association in a transaction, commits,
then drains cleanup. Return success after logical deletion even if physical
cleanup needs retry, consistent with existing attachment semantics. Never
delete attachments with raw SQL in game code: doing so bypasses the
attachment service's variant and physical cleanup logic.

## Upload and duplicate behavior

The browser offers `<input type="file" multiple>` and submits selected
files individually to the game screenshot upload endpoint. This is a batch
user operation with per-file requests, results, and progress. It preserves
bounded server streaming and isolates failed files without changing the
shared one-file attachment endpoint. Native no-JavaScript fallback accepts
repeated `files` parts in a single multipart request; process each completed
part through the same service operation.

For each file:

1. Validate session, owner, matching Origin, request type, and game existence
   before receiving or publishing bytes.
2. Stream through `AttachmentWriter`; apply existing filename and byte
   limits. Never trust the client MIME type or extension.
3. Finish with screenshot upload purpose and obtain the attachment record.
4. Resolve duplicates as described below before doing unnecessary conversion.
5. Generate thumbnail and preview variants through `ensureImageVariant()`.
6. Publish the association transaction only after both variants succeed.
7. On failure, drain the newly created unpublished attachment's queue entry.
   Never delete a preexisting attachment returned by deduplication.
8. Return a typed result containing screenshot identity and generated URLs.

Same attachment already associated with this game: return success with
`already_uploaded=true` and leave compression settings and upload timestamp unchanged.
Do not regenerate it merely because configuration changed.

Same attachment associated with another game: return 409 with a clear
message. The UNIQUE association avoids deleting another game's screenshot
when one is removed. Do not silently create overlapping ownership. Equal
bytes with a different filename remain separate attachment records under
the existing deduplication contract and may be used for different games.

An existing unattached attachment can be associated after both variants
succeed. Its owner must be told that removing the screenshot also deletes
that attachment record, potentially affecting links copied elsewhere.
No separate confirmation step is needed for each batch file; show this
lifetime rule alongside the upload control.

A duplicate record still awaiting cleanup returns 409; do not attach bytes
scheduled for deletion. Queue draining must be retried first. This also
covers uploading the same contents immediately after screenshot removal.

For native multipart batches, keep earlier completed successes when a
later file fails. Structural multipart corruption aborts the remaining
request; report completed results and the unprocessed remainder. Consume
recoverable invalid-file parts without buffering them. An interrupted
current writer is destroyed and its staging bytes are discarded. A file
whose association already committed stays published even if the browser
never receives the response; retry deduplication resolves this safely.

## HTTP and response contracts

Use the existing game route identity and URL prefix conventions:

| Method and route | Behavior |
| --- | --- |
| GET /games/:user/:id/screenshots | Public gallery and owner controls |
| POST /games/:user/:id/screenshots/upload | Owner upload; multipart |
| GET /games/:user/:id/screenshots/:shot/delete | Owner confirmation |
| POST /games/:user/:id/screenshots/:shot/delete | Delete and redirect |

Add a Screenshots link on the game detail page and a game-detail link on
the gallery. Unknown games and screenshot/game mismatches return 404.
Guests attempting management receive 401; authenticated nonowners receive
403, matching existing owner routes. Validate ownership before consuming
upload data. POST uses the existing exact Origin check and session refresh.

Each JavaScript request carries one `file` part and asks for JSON. Native
batch forms carry one or more `files` parts and request HTML. Reject mixed
field conventions, unknown fields, missing filenames, and no selected files.
Do not interpret a chooser cancellation as an empty screenshot.

A successful JSON response uses HTTP 200 and this shape:

```json
{
  "id": 42,
  "already_uploaded": false,
  "filename": "mountain.png",
  "thumbnail": {"url": "/attachment/<hash>/thumbnail.webp",
                "width": 320, "height": 180},
  "preview": {"url": "/attachment/<hash>/preview.webp",
            "width": 1920, "height": 1080},
  "original_url": "/attachment/<hash>/mountain.png"
}
```

Error responses contain a stable error code and a display-safe message.
Use 400 for malformed multipart/IDs, 409 for association conflicts, 413 for
upload limit violations, 415 for wrong request media type, 422 for invalid
names or unsupported/corrupt images, and generic 500 for storage failures.
Log internal details without returning filesystem paths or decoder traces.

Native batch results render a private, no-store result page with one
success/error per completed file and a link back to the gallery. Successful
native screenshot deletion redirects with 303. Management responses are
no-store. A public gallery containing owner controls must also be no-store;
do not share session-dependent responses through public caches. Immutable
image URLs retain the existing public attachment cache policy.

## Gallery and popup

Create `templates/games/screenshots.html` and module assets
`statics/games/screenshots.js` and `screenshots.css`. Reuse shared navigation,
head, metadata helpers, and the existing game owner presentation.

Use `display: flex`, `flex-wrap: wrap`, and consistent gaps on a container
that spans the page's available width. Do not inherit the journal sidebar
layout or a narrow article width. Preserve each thumbnail's aspect ratio,
cap its width to the viewport, and use stored width/height attributes to
reserve space. Use lazy loading and asynchronous decoding. Long filenames
wrap without expanding the viewport. Include a useful empty state.

Make each thumbnail an anchor to the preview compressed file. JavaScript
intercepts it to open one reusable native `<dialog>`, loads only the selected
preview variant, and sets the original link and filename. Without JavaScript,
the anchor opens the compressed preview image; a visible original link
remains available alongside each thumbnail.

The dialog contains a labeled close button, the preview image with contain
sizing within the viewport, and `Download original`. Restore focus to the
trigger on close. Support Escape and keyboard activation using native
controls. Clear the previous image and links before populating the next
selection, so failed loads never show stale screenshot metadata. Announce
loading/error states accessibly. Do not preload all preview images.
See [the dialog element](
https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/dialog).

Owner controls include batch selection, upload progress, per-file results,
and screenshot deletion links. Disable duplicate batch submission while
requests are active. Submit files sequentially initially, announce completed
counts, and allow retry of failed files. File-input `accept` is only a
selection aid; server validation is authoritative. Public markup escapes
filenames and uses URL helpers for every attachment basename.

## Public serving and privacy

Extend application fallback and `examples/apache_attachments.conf` with
fixed passive raster MIME rules for `.avif` and `.jxl`, preserving JPEG,
PNG, and WebP behavior. Generated suffixes must match the actual codec.
Continue hash-path validation, nosniff, and no directory/staging exposure.
Test both public-serving paths; successful conversion alone does not prove
the browser receives a displayable MIME type.

Original URLs remain public under the attachment model. The UI restriction
means the gallery does not embed originals; it does not make their URLs
private. Public cache copies may outlive deletion. The upload control states
that screenshots and originals are public. Preserve original bytes and
metadata exactly. Derivatives do not intentionally strip metadata or
profiles. The upload notice states that screenshots and originals are
public; this feature provides no metadata sanitization.

## Implementation sequence

1. Add configuration, normalization, codec availability checks, and AVIF/JXL
   conversion and public-serving support. Retain existing consumer behavior.
2. Create screenshot schema, owner enforcement, indexes, deletion trigger,
   and cleanup ledger after game and attachment schema initialization.
3. Extend screenshot-purpose upload finalization, association transactions,
   cleanup draining, and the screenshot service with injected dependencies.
4. Route game deletion through screenshot coordination and verify rollback
   and restart behavior before adding public upload routes.
5. Add owner routes and streaming processing, JSON and native batch results,
   and duplicate/conflict handling.
6. Add the gallery, dialog, owner controls, and game-detail navigation.
7. Update README, example configuration, deployment dependencies, and tests.

Follow repository naming/comment conventions, use libmw error and storage
helpers where applicable, and return `mw::E<>`-style errors. Keep Magick++
exceptions inside the existing conversion boundary. Do not add direct
encoder libraries when the system Magick++ coder supplies the functionality.

## Verification and acceptance

All writable tests use temporary roots and isolated databases. Never open
or modify the developer's `data.db`, `nsweekly.yaml`, or attachment directory.

Configuration/conversion tests cover aliases, invalid types and bounds,
unavailable configured coders, output signatures and MIME, portrait and
landscape images, EXIF orientation, transparency, no enlargement, resource
limits, unsupported inputs, and recipe-cache reuse. Test each requested
codec on a build supporting it; unavailable optional codecs are reported
explicitly in test output. Default WebP conversion tests must always run.

Database/service tests cover owner mismatch, duplicate uploads, conflicting
game associations, existing attachments, same bytes across users, both
stored integer compression settings, and exact original byte preservation. Verify records and
files rather than only HTTP status codes.

Deletion tests cover screenshot removal, direct attachment-manager removal,
and game cascade; each must leave no stale gallery rows. Verify cleanup
queue creation and draining, shared originals/outputs surviving when
referenced, last-reference cleanup, transaction rollback, unlink failure,
and restart after every publication/association/deletion boundary. Inject
failure between the two variant generations. Verify that existing
attachments survive failed screenshot association and newly created ones
are durably queued for cleanup.

Concurrency tests cover attachment deletion during conversion/association,
game deletion during upload, duplicate request retries after response loss,
and cleanup attempting to run while uploads are active. Assert that deleted
records cannot be resurrected and no active upload is reclaimed.

HTTP/browser tests cover guest reads, owner-only management, Origin checks,
malformed multipart, mixed fields, byte limits, partial native batches,
per-file JavaScript results, Unicode/long filenames, and empty galleries.
Verify full-width wrapping on narrow and wide screens, keyboard dialog
behavior, focus restoration, and no-JavaScript links/forms. Inspect network
requests: original bytes are fetched only after explicit original-link
activation. A broken or unsupported variant must never load the original
as a fallback.

Apache integration tests cover inline AVIF/JXL headers, unchanged arbitrary
file download policy, and public serving with the application stopped.
Check browser decode of configured codecs separately from server encoding.

Build with `cmake --build build -j24`, run the enabled CTest suite and
relevant browser/Apache checks, and confirm `BUILD_TESTING=OFF` preserves
the normal dependency footprint. No build is necessary merely to add this
design document.
