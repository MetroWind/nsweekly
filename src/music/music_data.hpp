#pragma once

#include <chrono>
#include <map>
#include <mutex>

#include "database.hpp"

// Metadata shared by completed and temporary listens.
struct MusicTrack
{
    // Trimmed display names and optionally the submitted release.
    std::string artist, title;
    std::optional<std::string> release;
    // Serialized additional_info, present only when supplied.
    std::optional<std::string> additional_info;
};

// A completed play and its stable position in history.
struct MusicListen
{
    // Client playback time in Unix seconds.
    int64_t listened_at;
    // Display and readback metadata.
    MusicTrack track;
    // Persistent ID assigned by storage.
    int64_t id = 0;
};

// A currently authorized device and the shared account it belongs to.
struct MusicIdentity
{
    // Persistent identifiers from MusicDevices and Users.
    int64_t device_id, owner_id;
    // NSWeekly account name from Users.
    std::string name;
};

// Settings information for a device, without its credential.
struct MusicDevice
{
    // The persistent device ID and the label chosen by its owner.
    int64_t id;
    std::string label;
    // Lifecycle timestamps in Unix seconds.
    int64_t created;
    std::optional<int64_t> revoked, last_submission;
};

// Inclusive lower and exclusive upper epoch-second boundaries.
struct MusicRange
{
    std::optional<int64_t> from, to;
};

// History bounds and the exclusive (timestamp, row ID) cursor.
struct MusicQuery
{
    // Selected date bounds and optional last displayed row.
    MusicRange range;
    std::optional<std::pair<int64_t, int64_t>> older;
    // Maximum rows returned.
    int limit = 25;
};

// A ranked artist or artist/title pair and its aggregate play count.
struct MusicRanking
{
    // Grouping names and the number of completed plays in the group.
    std::string artist, title;
    int64_t count;
};

// Counts and rankings over the entire selected date range.
struct MusicSummary
{
    // Total completed plays and top groups ordered by play count.
    int64_t count;
    std::vector<MusicRanking> artists, tracks;
};

// Monotonic clock for ephemeral playback and request ordering.
using MusicClock = std::chrono::steady_clock;

// Stores owner-scoped listening history, device credentials and playback.
class MusicDataInterface
{
public:
    // Allows destruction through the storage interface.
    virtual ~MusicDataInterface() = default;
    // Stores a new device and its credential digest.
    virtual E<void> createDevice(int64_t owner, const std::string& label,
                                 const std::string& digest, int64_t now) = 0;
    // Revokes a device belonging to the owner, including repeated requests.
    virtual E<bool> revokeDevice(int64_t owner, int64_t device, int64_t now) = 0;
    // Lists the owner's devices in creation order, newest first.
    virtual E<std::vector<MusicDevice>> devices(int64_t owner) = 0;
    // Resolves an active credential to the shared account and device.
    virtual E<std::optional<MusicIdentity>> identity(
        const std::string& digest) = 0;
    // Rechecks authorization and records an entire batch atomically.
    virtual E<void> complete(const MusicIdentity& device,
                             const std::vector<MusicListen>& listens,
                             int64_t received,
                             std::optional<MusicClock::time_point> single) = 0;
    // Rechecks authorization and replaces the device's temporary play.
    virtual E<void> playing(const MusicIdentity& device, MusicTrack track) = 0;
    // Finds the owner's most recently received active, unexpired play.
    virtual E<std::optional<MusicTrack>> nowPlaying(int64_t owner) = 0;
    // Reads a page of the owner's history.
    virtual E<std::vector<MusicListen>> history(int64_t owner,
                                               const MusicQuery& query) = 0;
    // Aggregates completed plays across the owner's selected range.
    virtual E<MusicSummary> summary(int64_t owner, const MusicRange& range) = 0;
};

// Owns a dedicated SQLite connection and serializes its use with playback.
class MusicDataSqlite : public MusicDataInterface
{
public:
    // Takes a configured connection and an optional monotonic clock for tests.
    explicit MusicDataSqlite(
        std::unique_ptr<SQLite> conn,
        MusicClock::time_point (*clock)() noexcept = MusicClock::now)
        : db(std::move(conn)), now(clock) {}
    // Initializes the Music tables after Users exists.
    E<void> initializeSchema();

    // Implements owner-scoped device and listen operations.
    E<void> createDevice(int64_t, const std::string&, const std::string&,
                         int64_t) override;
    E<bool> revokeDevice(int64_t, int64_t, int64_t) override;
    E<std::vector<MusicDevice>> devices(int64_t) override;
    E<std::optional<MusicIdentity>> identity(const std::string&) override;
    E<void> complete(const MusicIdentity&, const std::vector<MusicListen>&,
                     int64_t, std::optional<MusicClock::time_point>) override;
    E<void> playing(const MusicIdentity&, MusicTrack) override;
    E<std::optional<MusicTrack>> nowPlaying(int64_t) override;
    E<std::vector<MusicListen>> history(int64_t, const MusicQuery&) override;
    E<MusicSummary> summary(int64_t, const MusicRange&) override;

private:
    struct Playing
    {
        MusicClock::time_point received;
        uint64_t order;
        MusicTrack track;
    };

    void prunePlaying(MusicClock::time_point time);

    std::unique_ptr<SQLite> db;
    MusicClock::time_point (*now)() noexcept;
    std::mutex mutex;
    std::map<int64_t, Playing> playing_by_device;
    uint64_t playing_order = 0;
};
