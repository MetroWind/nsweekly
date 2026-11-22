#include <algorithm>

#include "music_data.hpp"

namespace
{
using namespace std::chrono_literals;

template<class... Columns, class... Args>
auto select(SQLite& db, const char* text, Args&&... args)
    -> E<std::vector<std::tuple<Columns...>>>
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(text));
    DO_OR_RETURN(sql.bind(std::forward<Args>(args)...));
    return db.eval<Columns...>(std::move(sql));
}

template<class... Args>
E<void> execute(SQLite& db, const char* text, Args&&... args)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(text));
    DO_OR_RETURN(sql.bind(std::forward<Args>(args)...));
    return db.execute(std::move(sql));
}

std::string rangeWhere(const MusicRange& range)
{
    std::string where = " WHERE owner_id=?1";
    // Keep the bounds indexable while reserving stable bind positions.
    where += range.from ? " AND listened_at>=?2" : " AND ?2 IS NULL";
    where += range.to ? " AND listened_at<?3" : " AND ?3 IS NULL";
    return where;
}

E<void> requireActive(SQLite& db, const MusicIdentity& device)
{
    ASSIGN_OR_RETURN(auto rows, select<int>(db, R"(
        SELECT 1 FROM MusicDevices
        WHERE id=? AND owner_id=? AND revoked IS NULL)",
        device.device_id, device.owner_id));
    if(rows.empty())
    {
        return std::unexpected(httpError(401, "Invalid device authorization"));
    }
    return {};
}

E<std::vector<MusicRanking>> rankings(SQLite& db, int64_t owner,
                                     const MusicRange& range, bool tracks)
{
    auto sql = std::string(tracks ? "SELECT artist, track, count(*)"
                                  : "SELECT artist, '', count(*)") +
               " FROM MusicListens" + rangeWhere(range) +
               (tracks
                ? " GROUP BY artist, track ORDER BY 3 DESC, track, artist"
                : " GROUP BY artist ORDER BY 3 DESC, artist") + " LIMIT 5";
    ASSIGN_OR_RETURN(auto rows,
        (select<std::string, std::string, int64_t>(
            db, sql.c_str(), owner, range.from, range.to)));
    std::vector<MusicRanking> result;
    for(auto& [artist, title, count] : rows)
    {
        result.push_back({std::move(artist), std::move(title), count});
    }
    return result;
}
}

E<void> MusicDataSqlite::initializeSchema()
{
    std::scoped_lock lock(mutex);
    SQLiteTransaction transaction(*db);
    DO_OR_RETURN(transaction.begin());
    for(const char* sql : {
        R"(CREATE TABLE IF NOT EXISTS MusicDevices(
            id INTEGER PRIMARY KEY,
            owner_id INTEGER NOT NULL REFERENCES Users(id),
            label TEXT NOT NULL,
            token_digest TEXT NOT NULL UNIQUE,
            created INTEGER NOT NULL,
            revoked INTEGER,
            last_submission INTEGER,
            UNIQUE(owner_id, id)))",
        R"(CREATE INDEX IF NOT EXISTS MusicDeviceOwner
            ON MusicDevices(owner_id, id DESC))",
        R"(CREATE TABLE IF NOT EXISTS MusicListens(
            id INTEGER PRIMARY KEY,
            owner_id INTEGER NOT NULL REFERENCES Users(id),
            device_id INTEGER NOT NULL,
            listened_at INTEGER NOT NULL,
            received INTEGER NOT NULL,
            artist TEXT NOT NULL CHECK(length(artist)>0),
            track TEXT NOT NULL CHECK(length(track)>0),
            release TEXT,
            additional_info TEXT CHECK(length(CAST(additional_info AS BLOB))
                                       <=10240),
            FOREIGN KEY(owner_id, device_id)
                REFERENCES MusicDevices(owner_id, id),
            UNIQUE(owner_id, listened_at, artist, track)))",
        R"(CREATE INDEX IF NOT EXISTS MusicListenOrder
            ON MusicListens(owner_id, listened_at DESC, id DESC))"})
    {
        DO_OR_RETURN(db->execute(sql));
    }
    return transaction.commit();
}

E<void> MusicDataSqlite::createDevice(int64_t owner, const std::string& label,
                                     const std::string& digest, int64_t created)
{
    std::scoped_lock lock(mutex);
    return execute(*db, R"(
        INSERT INTO MusicDevices(owner_id, label, token_digest, created)
        VALUES(?, ?, ?, ?))", owner, label, digest, created);
}

E<bool> MusicDataSqlite::revokeDevice(int64_t owner, int64_t device,
                                     int64_t revoked)
{
    std::scoped_lock lock(mutex);
    ASSIGN_OR_RETURN(auto rows, select<int64_t>(*db, R"(
        UPDATE MusicDevices SET revoked=coalesce(revoked, ?)
        WHERE owner_id=? AND id=? RETURNING id)", revoked, owner, device));
    if(rows.empty())
    {
        return false;
    }
    prunePlaying(now());
    playing_by_device.erase(device);
    return true;
}

E<std::vector<MusicDevice>> MusicDataSqlite::devices(int64_t owner)
{
    std::scoped_lock lock(mutex);
    ASSIGN_OR_RETURN(auto rows,
        (select<int64_t, std::string, int64_t, std::optional<int64_t>,
                std::optional<int64_t>>(*db, R"(
            SELECT id, label, created, revoked, last_submission
            FROM MusicDevices WHERE owner_id=? ORDER BY id DESC)", owner)));
    std::vector<MusicDevice> result;
    for(auto& [id, label, created, revoked, last] : rows)
    {
        result.push_back({id, std::move(label), created, revoked, last});
    }
    return result;
}

E<std::optional<MusicIdentity>>
MusicDataSqlite::identity(const std::string& digest)
{
    std::scoped_lock lock(mutex);
    ASSIGN_OR_RETURN(auto rows, (select<int64_t, int64_t, std::string>(*db, R"(
        SELECT d.id, d.owner_id, u.name FROM MusicDevices d
        JOIN Users u ON u.id=d.owner_id
        WHERE d.token_digest=? AND d.revoked IS NULL)", digest)));
    if(rows.empty())
    {
        return std::nullopt;
    }
    auto& [id, owner, name] = rows.front();
    return MusicIdentity{id, owner, std::move(name)};
}

E<void> MusicDataSqlite::complete(
    const MusicIdentity& device, const std::vector<MusicListen>& listens,
    int64_t received, std::optional<MusicClock::time_point> single)
{
    std::scoped_lock lock(mutex);
    SQLiteTransaction transaction(*db);
    DO_OR_RETURN(transaction.begin());
    DO_OR_RETURN(requireActive(*db, device));
    for(const auto& listen : listens)
    {
        const auto& track = listen.track;
        DO_OR_RETURN(execute(*db, R"(
            INSERT INTO MusicListens(owner_id, device_id, listened_at, received,
                                     artist, track, release, additional_info)
            VALUES(?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(owner_id, listened_at, artist, track) DO NOTHING)",
            device.owner_id, device.device_id, listen.listened_at, received,
            track.artist, track.title, track.release, track.additional_info));
    }
    DO_OR_RETURN(execute(*db, R"(
        UPDATE MusicDevices SET last_submission=max(coalesce(last_submission, ?), ?)
        WHERE id=? AND owner_id=?)", received, received,
        device.device_id, device.owner_id));
    DO_OR_RETURN(transaction.commit());

    prunePlaying(now());
    auto entry = playing_by_device.find(device.device_id);
    if(single && listens.size() == 1 && entry != playing_by_device.end() &&
       entry->second.received < *single &&
       entry->second.track.artist == listens.front().track.artist &&
       entry->second.track.title == listens.front().track.title)
    {
        playing_by_device.erase(entry);
    }
    return {};
}

void MusicDataSqlite::prunePlaying(MusicClock::time_point time)
{
    std::erase_if(playing_by_device, [time](const auto& entry)
    {
        return time - entry.second.received >= 5min;
    });
}

E<void> MusicDataSqlite::playing(const MusicIdentity& device, MusicTrack track)
{
    std::scoped_lock lock(mutex);
    DO_OR_RETURN(requireActive(*db, device));
    auto received = now();
    prunePlaying(received);
    playing_by_device.insert_or_assign(
        device.device_id, Playing{received, ++playing_order, std::move(track)});
    return {};
}

E<std::optional<MusicTrack>> MusicDataSqlite::nowPlaying(int64_t owner)
{
    std::scoped_lock lock(mutex);
    prunePlaying(now());
    if(playing_by_device.empty())
    {
        return std::nullopt;
    }
    ASSIGN_OR_RETURN(auto rows, select<int64_t>(*db, R"(
        SELECT id FROM MusicDevices WHERE owner_id=? AND revoked IS NULL)",
        owner));
    const Playing* latest = nullptr;
    for(auto [id] : rows)
    {
        auto entry = playing_by_device.find(id);
        if(entry != playing_by_device.end() &&
           (!latest || entry->second.order > latest->order))
        {
            latest = &entry->second;
        }
    }
    return latest ? std::optional{latest->track} : std::nullopt;
}

E<std::vector<MusicListen>> MusicDataSqlite::history(int64_t owner,
                                                   const MusicQuery& query)
{
    std::scoped_lock lock(mutex);
    auto sql = "SELECT listened_at, artist, track, release, additional_info, id"
               " FROM MusicListens" + rangeWhere(query.range);
    sql += query.older ? " AND (listened_at, id)<(?4, ?5)"
                       : " AND ?4 IS NULL AND ?5 IS NULL";
    sql += " ORDER BY listened_at DESC, id DESC LIMIT ?6";
    std::optional<int64_t> timestamp, id;
    if(query.older)
    {
        timestamp = query.older->first;
        id = query.older->second;
    }
    ASSIGN_OR_RETURN(auto rows,
        (select<int64_t, std::string, std::string, std::optional<std::string>,
                std::optional<std::string>, int64_t>(
            *db, sql.c_str(), owner, query.range.from, query.range.to,
            timestamp, id, query.limit)));
    std::vector<MusicListen> result;
    for(auto& [time, artist, title, release, extra, row_id] : rows)
    {
        result.push_back({time, {std::move(artist), std::move(title),
                                std::move(release), std::move(extra)}, row_id});
    }
    return result;
}

E<MusicSummary> MusicDataSqlite::summary(int64_t owner, const MusicRange& range)
{
    std::scoped_lock lock(mutex);
    auto sql = "SELECT count(*) FROM MusicListens" + rangeWhere(range);
    ASSIGN_OR_RETURN(auto rows,
        select<int64_t>(*db, sql.c_str(), owner, range.from, range.to));
    ASSIGN_OR_RETURN(auto artists, rankings(*db, owner, range, false));
    ASSIGN_OR_RETURN(auto tracks, rankings(*db, owner, range, true));
    return MusicSummary{std::get<0>(rows.front()), std::move(artists),
                        std::move(tracks)};
}
