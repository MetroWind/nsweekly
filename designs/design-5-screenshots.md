# Game screenshots

Status: revised proposed design, 2026-10-12. This document specifies
implementation work and the upload lifecycle contract.

This revision replaces screenshot-purpose attachment uploads with
consumer-defined `Upload` tasks executed by the attachment module. It also
separates generic provisional-file recovery from screenshot deletion cleanup.
JPEG XL color-profile preservation and verification remain deferred by
agreement; the other output codecs retain the color requirements below.

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
contract. Batch screenshot uploads use a game-specific endpoint. For each
file, the games module constructs a derived `Upload` task and transfers it
to the attachment executor. It does not call task lifecycle hooks or make
internal HTTP requests. The existing writer is an internal streaming
building block; its current immediate-publication `finish()` is not the
publication boundary for a consumer task.

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

For JPEG, WebP, and AVIF, preserve embedded ICC color profiles in
thumbnails and previews by default.
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
The color-management requirements in this section apply to JPEG, WebP, and
AVIF; the following JPEG XL exception takes precedence for that codec.
Run these color checks for JPEG, WebP, and AVIF. JPEG XL uses the standard
system ImageMagick encoding path for now: do not add a JXL-specific profile
workaround or reject its output because the encoder changes or drops an ICC
profile. Profile and decoded-color checks for JXL are deferred. Signature,
readability, dimensions, orientation, supported non-color metadata, quality,
resource limits, and original-byte checks still apply to JXL. This exception
must be documented as a current limitation rather than claiming equal color
preservation across all codecs.

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

This is exclusively a consumer-owned deletion queue. Uploads never enter
it. New attachment records and screenshot associations commit together, so
there is no published-but-unassociated screenshot attachment to track.
Interrupted provisional files belong to generic attachment recovery, which
is described below. Keep these two responsibilities separate: one releases
published consumer references, while the other reclaims incomplete file work.

Queue entries contain stable owner IDs, not filenames or public URLs.
They are private operational metadata and are never rendered publicly.

## Consumer-defined upload tasks

### Responsibility and ownership

Define `Upload` in the attachment module. A consumer derives a task class,
initializes its module-specific state in the constructor, and passes a
`std::unique_ptr<Upload>` to the attachment service. The service executes
the task synchronously and owns it through completion or failure. A task
represents one file and is executed at most once.

Use ownership transfer by value, for example:

```cpp
mw::E<UploadResult> doUpload(
    std::unique_ptr<Upload> upload_task);
```

The caller uses `std::move()` for a named task. Passing `unique_ptr` by
value transfers ownership without requiring an rvalue-reference parameter.
The task base needs a public virtual destructor. Neither task nor result
needs shared ownership. Request readers and injected services borrowed by a task
must outlive execution. `doUpload()` executes synchronously and does not
start detached work. The callback-streaming adapter described below also
keeps execution inside the request lifetime.

`UploadResult` is one concrete value type defined in the attachment module.
Every upload returns the same type regardless of which module initiated it.
It contains a snapshot of the published attachment information: record ID,
owner ID, validated filename, source hash, MIME type, and size. It may reuse
an existing attachment metadata value type. It has no virtual methods,
consumer-specific fields, or reference to task-owned state. The executor
constructs it from the resolved attachment and returns it only after commit.
Consumers do not derive result classes or perform runtime result type checks.

The screenshot service uses the returned attachment identity and its known
game context to query the committed screenshot association and build the
HTTP response, including stored settings and generated gallery URLs. The
existing `already_uploaded` response flag describes whether the screenshot
association already existed, which cannot be inferred from attachment
reuse: an existing unattached attachment can receive a new association.
For that flag, the screenshot service owns a request-local boolean and
passes a reference to its task. The end hook sets it from its authoritative
association check. This is consumer state, not an alternative upload result
type; the attachment module never inspects it. The service uses it only
after successful execution and discards it on failure. Its lifetime must
cover synchronous execution or the full streaming handle lifetime. A later
response-building failure does not undo a committed upload; a retry resolves
the existing association through the normal duplicate rules.

### Abstract hooks and service-owned lifecycle implementation

The three lifecycle hooks are public and pure virtual. They contain only
module-specific logic. Consumers override them but submit tasks to the
attachment service instead of invoking the hooks themselves. This calling
convention is an API contract rather than a C++ access restriction. Common
logic belongs to private, nonvirtual `AttachmentService` functions, which operate on per-upload
`UploadExecution` state and invoke the hooks at the appropriate points.
The service never switches on consumer types.

`Upload` is the consumer extension interface, not the lifecycle driver. It
contains consumer-specific state in derived classes and may hold immutable
`UploadInput` until the service transfers that input into execution state.
It owns no staging files, hash accumulator, active-work registration,
transaction, or lifecycle phase. `UploadExecution` owns those per-upload
resources and the task; `AttachmentService` implements their lifecycle.

The following interface is schematic; context contents are specified in the
next section. Public interface items must have intention comments in the
implementation.

```cpp
// Consumer-supplied work executed exclusively by the attachment service.
class Upload
{
public:
    // Allows deletion through an owning base pointer.
    virtual ~Upload() = default;
    Upload(const Upload&) = delete;
    Upload& operator=(const Upload&) = delete;

    // Transfers initial request input into service-owned execution state.
    mw::E<UploadInput> takeInput();
    // Validates consumer state before the service receives file bytes.
    virtual mw::E<void> beginUpload(UploadStart& context) = 0;
    // Adds consumer processing after the original is completely staged.
    virtual mw::E<void> process(UploadProcessing& context) = 0;
    // Writes consumer records within the service's publication transaction.
    virtual mw::E<void> endUpload(UploadPublication& context) = 0;

protected:
    // Captures initial request input for transfer into execution state.
    explicit Upload(UploadInput input);

private:
    UploadInput input;
};
```

`Upload` has no friend declarations. The service calls its public virtual
hooks and uses `takeInput()` to move the initial input into execution state.
Input transfer occurs once, before common setup and `beginUpload()`; callers
must not consume input before submitting the task. The implementation must
reject reuse of a task whose input has already been transferred.
`UploadExecution` never invokes the hooks. Its public streaming methods are
thin delegates to private service operations. The service may grant the
execution handle access to those operations; this is separate from the
public task interface. The service must outlive every execution handle that
borrows it.

The private service operations perform common work around the virtual hooks.
Each takes an `UploadExecution&`; their exact signatures may follow existing
service conventions:

| Private service operation | Common logic | Consumer hook |
| --- | --- | --- |
| `startExecution()` | Validate owner/name and storage; create staging and register active work | Call `beginUpload()` before consuming bytes |
| `receive()` | Enforce byte limits; stream and hash chunks into private staging | No consumer hook needed for common reception |
| `prepare()` | Finalize the staged digest and MIME; expose bounded conversion and duplicate lookup helpers | Call `process()` with the immutable staged source |
| `publish()` | Lock attachment mutations; begin transaction; revalidate identity; install files and attachment metadata; commit or roll back | Call `endUpload()` before committing |
| `complete()` | Enforce phase order and single completion; run preparation and publication; own failure cleanup and result lifetime | Never requires the consumer to chain hooks |
| `cancel()` | Reclaim or register recovery of provisional resources; unregister active work and destroy the task | No additional consumer hook |

Consumers do not override these service operations, handle attachment
transactions, or call base hook implementations. An ordinary attachment task
has empty processing and end hooks; common validation, streaming,
publication, and result construction still run. Existing attachment-manager
and journal upload behavior is preserved through that task, rather than
maintaining a second immediate-publication path for HTTP uploads.

### Hook contexts and the byte source

`UploadInput` carries the owner ID, supplied filename, and, for synchronous
execution, a request-scoped bounded byte source. The attachment module
defines reception in terms of chunks, completion, and errors, without
depending on games or multipart field names. EOF means the current file
part completed structurally. Cancellation or malformed multipart is an
error, not EOF. Streaming applies existing filename and upload-byte limits;
it never collects the original into a string or an image BLOB.

The current HTTP multipart parser pushes chunks through callbacks. A
synchronous `doUpload()` must not try to pull from that parser recursively
inside a part-header callback. Provide an attachment-owned execution handle
for this adapter:

```cpp
// Transfers a task and runs common start logic and its begin hook.
mw::E<std::unique_ptr<UploadExecution>> startUpload(
    std::unique_ptr<Upload> upload_task);
```

`UploadExecution` owns the task, staging files, hash accumulator, byte
count, lifecycle phase, and active-work registration. It borrows the
attachment service. Its public `append(bytes)` delegates bounded reception
to the service; its public `finish()` delegates preparation and publication
and returns the same result type as `doUpload()`. Destruction before
successful completion delegates cancellation to the service. Cancellation
must not throw; failed filesystem cleanup remains recoverable through the
generic manifest mechanism. The service releases active-work registration
only after cleanup succeeds or recovery can safely take over.

A terminal error or a completed `finish()` destroys the task before
returning; the remaining handle is inert. Consumer wrappers release their
coordination resources on completion, cancellation, or terminal error. These handle operations never call the consumer's hooks
directly. Consumers never retain or call a task after transferring it to
either entrypoint.

At each part header, the HTTP adapter constructs the derived task and calls
`startUpload()` before any bytes for that file are accepted. Subsequent data
callbacks call the handle's `append()`. Call `finish()` only when the parser
proves that the part is structurally complete. For a native batch, the next
part header completes the previous part, and successful end-of-request
completes the last one. For a JSON request, defer `finish()` until the entire
request has been validated as exactly one file; a second part or mixed field
convention cancels the handle without publication. Recoverable per-file
errors consume the remaining part and continue according to batch policy;
structural corruption discards the current handle and aborts the remainder.

`doUpload()` is the synchronous convenience wrapper around this same
execution handle: start, stream its supplied source, and finish. Both public
entrypoints use the same private service operations, which invoke the
public hooks in the same order. There is one implementation of common upload mechanics.
The existing writer may be reused internally, but closing its staging and
computing a digest must be separated from committing an attachment record.
Early session, Origin, owner, and game validation still occurs before the
HTTP body reader; the begin hook revalidates the consumer state before
accepting that part's bytes.

`UploadStart` supplies immutable request identity and consumer validation
inputs. `ScreenshotUpload::beginUpload()` verifies the owner-scoped game
through game storage. Authorization must precede streaming even when no
attachment records will be published until later.

`UploadProcessing` exposes immutable source identity and narrow generic
operations: inspect the staged source, look up a possible existing named
attachment, resolve cached variants, and prepare a variant using
`ImageVariantOptions`. Its helpers own file descriptors, conversion limits,
and staging. Consumers cannot unlink files or publish attachment records.
Prepared variants stay provisional; `process()` is not allowed to call the
current public `ensureImageVariant()` on a newly created attachment, since
there is no published attachment record yet. Refactor the shared conversion
implementation so both public conversion and staged preparation use the
same recipe and encoder helpers.

Duplicate lookups during processing are advisory snapshots. They avoid
unnecessary conversion but do not reserve a record or guarantee that it will
survive until publication. Cached files used outside the mutation lock need
a pinned descriptor or a private staged copy. A cache hit observed during
processing must be rechecked at publication; if it disappeared, fail with
a retryable conflict or prepare it again outside the transaction. Never
encode while holding the publication transaction open.

`UploadPublication` contains the resolved owner-scoped attachment record,
its newly-created/previously-existing indication, prepared or reused variant
metadata, and a borrowed transaction-scoped database access object. It
supplies generic attachment lookups needed to revalidate duplicates. The
context is valid only inside `endUpload()` and cannot escape in a result.

The database access object borrows the attachment connection already in its
write transaction. Games data helpers accept this access object and execute
only consumer-table queries and writes through it. They must not call normal
game storage methods that open another connection or begin another
transaction. The executor owns `BEGIN`, `COMMIT`, and `ROLLBACK`; hooks must
not issue transaction-control statements, call locking attachment service
operations recursively, or mutate attachment tables directly. This is an
internal programming contract, not an isolation boundary for untrusted
plugins. A restricted transaction view should expose parameterized query
and execution operations without exposing transaction control as methods.
Statements must belong to the borrowed connection; reject statements
prepared on another connection before executing them.

For schema initialization, gallery reads, and deletion, generic service
`readMetadata()` and `writeMetadata()` operations lend the same restricted
view to games-owned helpers under the attachment mutation mutex.
`writeMetadata()` also owns a transaction. Neither operation embeds consumer
SQL in the attachment module. They must not be called recursively from a
publication hook; that hook already has the appropriate view.

### Publication and crash recovery

No attachment record or variant mapping for a new upload is committed
before `process()` succeeds. The publication sequence is:

1. Finish reception, hashing, MIME probing, and all requested conversions
   in private staging. No SQLite write transaction or attachment mutation
   mutex is held while receiving bytes or encoding images.
2. Acquire the attachment mutation mutex and start a write transaction on
   its connection. Resolve deduplication again using owner, hash, and exact
   validated filename. Revalidate any record identity observed earlier;
   an attachment deleted during processing must not be resurrected under
   that old identity. An expected existing record that disappeared causes
   a conflict rather than being silently replaced.
3. Install original and derivative bytes through the generic attachment
   publication helpers, reusing equal hashes safely. Insert or reuse
   attachment records and recipe mappings in this still-uncommitted
   transaction. Files must be durable before their records commit.
4. Call the task's `endUpload()` with the publication context. The screenshot
   override checks the game, owner, deletion queue, and association again,
   then inserts its screenshot row or accepts an existing same-game
   association. All consumer writes use this exact connection and transaction.
5. Commit only when the hook and every pre-commit publication operation
   have succeeded. The executor constructs the common attachment result
   before commit and returns it only after commit succeeds and the mutex
   is released. A hook error, storage error, or commit failure
   rolls back both attachment and consumer metadata and discards the result.
   After a successful commit, a manifest/staging cleanup failure is logged
   and retried; it must not turn the committed upload into a failed result.

SQLite transactions cannot atomically roll back filesystem operations.
Files installed before a failed commit may remain as unreferenced hashes;
this is acceptable only with generic recovery. Before installing any new
hash paths, write and fsync an attachment-owned publication manifest in
private staging listing those hashes. Write a temporary manifest, fsync it,
atomically rename it, and fsync the staging directory before installing any
public hashes. A crash during manifest creation therefore leaves only
ordinary abandoned staging, never a partially written recovery manifest.
Keep the manifest until commit succeeds and cleanup bookkeeping is durable. On failure or restart, use the existing
attachment reference checks to reclaim only unreferenced paths. Never unlink
a hash merely because the failed task mentioned it: another attachment or
variant may reference the same physical bytes.

A manifest is generic operational metadata: task identity and file hashes,
with no module name, game ID, screenshot purpose, or consumer SQL. In-process
recovery excludes active tasks under the attachment service's own registry
and mutation mutex. Startup recovery runs before requests are accepted, so
interrupted staging and manifests have no live task owners. Failed unlinks
retain durable recovery work for retry. A crash after commit but before
manifest removal is safe: reference checks preserve the committed files.
Reuse the existing attachment recovery algorithm where possible and extend
its staging handling to recognize per-task staging files and manifests.

Metadata publication is atomic; public file visibility is not. Apache serves
hash paths without querying SQLite, so installed bytes can be retrievable
by a known hash before commit and may survive temporarily after rollback.
Do not claim provisional bytes become private or that a database rollback
withdraws a previously served copy. No gallery or attachment manager record
is returned or rendered until the metadata transaction commits.

The final consumer write must share the attachment connection. Separate
transactions on two connections to the same database do not provide this
atomicity. Follow SQLite's [transaction semantics](
https://www.sqlite.org/lang_transaction.html), including rollback after a
failed commit and the existing busy-error policy. If rollback cannot restore
an idle connection, invalidate it under the current storage policy.

## Screenshot service and deletion boundaries

Add `GameScreenshotService`, borrowing `AttachmentService` and game storage.
It constructs `ScreenshotUpload` tasks, transfers them to the attachment
executor, resolves public gallery metadata, and coordinates screenshot/game
deletion. `GamesModule`
borrows this service. `ScreenshotUpload` and its SQL helpers live in games;
the attachment module knows neither their type nor their tables.

`ScreenshotUpload::process()` resolves advisory duplicates and prepares the
two variants sequentially through generic processing-context helpers. For
an observed same-game duplicate, it may skip conversion; `endUpload()` must
revalidate that same association and preserve its stored settings. If that
association disappeared, return a conflict and let a retry prepare a fresh
upload outside the transaction. For an existing unattached attachment, stage
missing variants without changing the existing record until publication.

`ScreenshotUpload::endUpload()` performs these consumer operations through
the supplied publication transaction:

1. Resolve the game and verify its owner equals the attachment owner. A
   deleted game fails with 404; owner mismatch fails authorization.
2. Reject any attachment still in `GameScreenshotCleanup`. There is no
   exception for an operation's own unpublished upload: uploads never use
   that queue.
3. Resolve the attachment's association. Another game returns 409. An
   existing same-game association sets the consumer-owned duplicate flag
   and preserves its identity, timestamp, settings, and variant metadata.
4. For a new association, validate the selected typed settings and verify
   that both requested variant mappings exist in this transaction. Insert
   the screenshot through games-owned data helpers. The insertion trigger
   independently enforces owner equality.
5. Set the consumer-owned duplicate flag to false for a new association
   and return success. The executor constructs the common `UploadResult`
   and commits the shared transaction before returning it to the caller.
   The screenshot service then resolves the committed association for its
   HTTP response using the returned attachment ID and known game ID.

An advisory duplicate can become associated by another request during
processing. Handle that as the same duplicate/conflict cases in the final
transaction. Derivatives prepared unnecessarily may be discarded by generic
attachment cleanup; do not rewrite an existing screenshot's recipes or
attach new outputs to it merely because current configuration differs.

The screenshot service serializes upload, screenshot deletion, game deletion,
and screenshot deletion-queue draining within one process. Initially hold
its coordinator lock through the complete `doUpload()` call, or from
`startUpload()` until the corresponding execution handle is finished or
cancelled, including resolution of the committed screenshot response.
Acquire this lock in the screenshot service before transferring the task,
and let the games-owned streaming wrapper retain the moved lock. It holds
that lock through response mapping so game deletion cannot remove the
association between publication and the wrapper's metadata query. A terminal
append error or failed finish releases it; destruction cancels the execution
handle before releasing it. Task destruction and consumer output lifetime
remain independent of that lock. This coordination lock
belongs to the consumer; generic attachment execution does not acquire or
interpret it. Always acquire the consumer coordinator before the attachment
mutation mutex. Ordinary attachments and public reads remain independent.
All game-delete routes use the screenshot service's deletion entrypoint.
Direct attachment-manager deletion still uses the attachment service and
its mutex; publication rechecks attachment identity to handle that race.

Screenshot deletion removes its association in a game transaction and
commits before draining `GameScreenshotCleanup`. Game deletion removes
`GameTracking`; its cascade removes screenshot rows and triggers queue
insertion in the same game transaction. A rollback restores the game,
screenshots, and queue state together. A crash after commit leaves a durable
consumer-owned deletion queue.

The games module enumerates its queue and invokes the generic
`deleteAttachment(user_id, attachment_id)` operation after logical deletion.
A successful deletion cascades away the queue row. Treat an already missing
attachment as complete; leave storage failures queued and log them. Drain
at startup before accepting requests and after screenshot/game mutations.
The attachment service never enumerates this queue or executes its SQL.
Logical deletion succeeds even if physical reclamation needs retry, matching
existing attachment semantics. Games code never deletes attachment rows
with raw SQL or manipulates attachment files.

## Effect on the previous implementation proposal

Do not add `AttachmentPurpose`, screenshot flags on generic writers,
`AttachmentService::associateScreenshot()`, screenshot listing/deletion
methods on the attachment service, or attachment-side SQL mentioning
`GameScreenshots` or `GameScreenshotCleanup`. Replace those paths in the
previous implementation when implementation work resumes. Merely wrapping
the old finish-and-associate sequence in a derived class does not solve the
publication gap.

The intended boundary is: the attachment module executes an abstract task
and owns generic file/metadata mechanics; the consumer's hooks own
authorization, processing choices, and consumer-table writes. Upload recovery
is generic, while release of published screenshot attachments remains games
policy.

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
2. Construct a `ScreenshotUpload` task with the owner, game, compression
   settings, and source identity. Transfer its `unique_ptr` to `doUpload()`
   or the callback-streaming `startUpload()` adapter; the consumer never
   calls its lifecycle hooks directly.
3. The attachment service's private start/preparation functions validate
   and stream the original, then invoke `process()` to check advisory duplicates and
   prepare thumbnail and preview variants. No new attachment is committed.
4. The attachment service's private publication function starts the shared
   transaction, publishes attachment metadata, invokes `endUpload()` for screenshot
   association, and commits both together.
5. On failure, roll back metadata and reclaim execution-owned provisional files
   through generic attachment recovery. Existing attachment records survive;
   no screenshot upload is inserted into the screenshot deletion queue.
6. Use the common attachment result to resolve the committed screenshot
   association and generate the HTTP response and URLs. If the response
   is lost, retry resolves the existing association safely.

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
current staged source is discarded without running its publication hook. A file
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
profiles; JPEG XL profile preservation remains subject to the explicit
deferral above. The upload notice states that screenshots and originals
are public; this feature provides no metadata sanitization.

## Implementation sequence

1. Add configuration, normalization, codec availability checks, and AVIF/JXL
   conversion and public-serving support. Retain existing consumer behavior.
2. Introduce generic `Upload` tasks, public abstract hooks, private service
   lifecycle functions, execution-owned state, bounded streaming adapters,
   publication contexts, and generic crash recovery. Adapt ordinary attachment uploads to this
   executor and verify their existing contract.
3. Create screenshot schema, owner enforcement, indexes, deletion trigger,
   and the consumer-owned deletion queue after game/attachment initialization.
   Add `ScreenshotUpload`, games-owned transaction helpers, response mapping,
   and `GameScreenshotService`. Verify atomic publication before HTTP work.
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
stored integer compression settings, and exact original byte preservation.
Verify records and files rather than only HTTP status codes.

Deletion tests cover screenshot removal, direct attachment-manager removal,
and game cascade; each must leave no stale gallery rows. Verify cleanup
queue creation and draining, shared originals/outputs surviving when
referenced, last-reference cleanup, transaction rollback, unlink failure,
and restart after every staging/publication/association/deletion boundary.
Inject failure between the two variant generations, inside `endUpload()`,
after file installation, and during commit. Verify that new attachment and
screenshot records are both absent after rollback, existing attachments
survive failed association, and generic manifests reclaim unreferenced
files without consulting consumer tables. Uploads must never enter
`GameScreenshotCleanup`; it contains only release work from deletions.

Concurrency tests cover attachment deletion during conversion/association,
game deletion during upload, duplicate request retries after response loss,
and generic recovery attempting to run while uploads are active. Assert
that deleted records cannot be resurrected, no active staging is reclaimed,
and a stale duplicate/cache snapshot is revalidated before commit.

Task-contract tests verify that the hooks are public and pure virtual,
`Upload` has no friend declarations, consumers submit tasks instead of
calling hooks directly, and all three hooks contain only consumer logic.
Private service lifecycle
functions enforce their order for both synchronous and callback-streaming
execution. Verify that `Upload` has no common lifecycle operations or
mutable execution resources, and that only `AttachmentService` invokes its
hooks. The execution handle must delegate reception, completion, and
cancellation to the service, which outlives the handle. Reject duplicate
finish calls, appends after completion, and a JSON request containing a second file without publishing the first one.
Verify that a start error prevents reception, a processing error prevents
publication, and an end-hook error rolls back
both modules' writes. The end hook must use the supplied transaction view;
tests must expose any attempted second-connection write or nested transaction.
Verify that tasks are destroyed on failure and the common value result
is returned only after successful commit. All consumers receive the same
concrete result type without casts or result subclasses. Verify screenshot
response mapping and distinguish an existing unattached attachment from
an existing screenshot association when setting `already_uploaded`. Discard
consumer-owned output state on failure and verify its lifetime covers
streaming execution. Ordinary uploads must execute without importing games
headers or referring to games schema, purpose values, or deletion queues.

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
