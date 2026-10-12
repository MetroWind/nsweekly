#include "games/game_screenshot_data.hpp"
#include "attachment/image_variant.hpp"

namespace
{
template <class... Args>
E<void> execute(UploadDatabase& db, const char* text, Args... args)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(text));
    DO_OR_RETURN(sql.bind(args...));
    return db.execute(std::move(sql));
}

E<ImageVariant> variant(UploadDatabase& db, const std::string& source,
                        const std::string& recipe, const std::string& name)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(R"(
SELECT f.hash,f.mime_type,f.byte_size,f.created,v.width,v.height,v.format
 FROM AttachmentVariants v JOIN AttachmentFiles f ON f.hash=v.output_hash
 WHERE v.source_hash=? AND v.recipe=?)"));
    DO_OR_RETURN(sql.bind(source, recipe));
    ASSIGN_OR_RETURN(auto rows,
                     (db.eval<std::string, std::string, int64_t, int64_t, int,
                              int, std::string>(std::move(sql))));
    if(rows.empty())
    {
        return std::unexpected(runtimeError("Missing screenshot variants"));
    }
    auto& [hash, mime, size, created, width, height, format] = rows[0];
    ASSIGN_OR_RETURN(
        auto url,
        attachmentURL(hash, name + "." + (format == "jpeg" ? "jpg" : format)));
    return ImageVariant{
        {hash, mime, size, created}, format, url, width, height};
}
} // namespace

ImageVariantOptions screenshotOptions(const ScreenshotSettings& settings)
{
    return {settings.long_edge, settings.long_edge, settings.quality,
            screenshotFormat(settings.codec)};
}

E<void> validateScreenshotEncoders(const Configuration& config)
{
    for(auto settings :
        {config.screenshot_thumbnail, config.screenshot_preview})
    {
        DO_OR_RETURN(validateImageEncoder(screenshotOptions(settings)));
    }
    return {};
}

E<void> initializeScreenshotSchema(UploadDatabase& db)
{
    for(const auto sql : {
            R"(CREATE TABLE IF NOT EXISTS GameScreenshots(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 game_id INTEGER NOT NULL REFERENCES GameTracking(id) ON DELETE CASCADE,
 attachment_id INTEGER NOT NULL UNIQUE
 REFERENCES Attachments(id) ON DELETE CASCADE,
 thumbnail_codec INTEGER NOT NULL CHECK(typeof(thumbnail_codec)='integer' AND thumbnail_codec BETWEEN 0 AND 3),
 thumbnail_size INTEGER NOT NULL CHECK(typeof(thumbnail_size)='integer' AND thumbnail_size>0),
 thumbnail_quality INTEGER NOT NULL CHECK(typeof(thumbnail_quality)='integer' AND thumbnail_quality BETWEEN 1 AND 100),
 preview_codec INTEGER NOT NULL CHECK(typeof(preview_codec)='integer' AND preview_codec BETWEEN 0 AND 3),
 preview_size INTEGER NOT NULL CHECK(typeof(preview_size)='integer' AND preview_size>0),
 preview_quality INTEGER NOT NULL CHECK(typeof(preview_quality)='integer' AND preview_quality BETWEEN 1 AND 100),
 added INTEGER NOT NULL CHECK(typeof(added)='integer'), CHECK(thumbnail_size<=preview_size)))",
            R"(CREATE INDEX IF NOT EXISTS GameScreenshotOrder
 ON GameScreenshots(game_id,added DESC,id DESC))",
            R"(CREATE TABLE IF NOT EXISTS GameScreenshotCleanup(
 attachment_id INTEGER PRIMARY KEY REFERENCES Attachments(id) ON DELETE CASCADE,
 user_id INTEGER NOT NULL REFERENCES Users(id)))",
            R"(CREATE TRIGGER IF NOT EXISTS GameScreenshotOwner
 BEFORE INSERT ON GameScreenshots
 WHEN NOT EXISTS(SELECT 1 FROM GameTracking g JOIN Attachments a
 ON a.user_id=g.user_id WHERE g.id=NEW.game_id AND a.id=NEW.attachment_id)
 BEGIN SELECT RAISE(ABORT,'Screenshot owner mismatch'); END)",
            R"(CREATE TRIGGER IF NOT EXISTS GameScreenshotImmutable
 BEFORE UPDATE ON GameScreenshots
 BEGIN SELECT RAISE(ABORT,'Screenshots are immutable'); END)",
            R"(CREATE TRIGGER IF NOT EXISTS GameScreenshotRelease
 BEFORE DELETE ON GameScreenshots BEGIN
 INSERT OR IGNORE INTO GameScreenshotCleanup(attachment_id,user_id)
 SELECT id,user_id FROM Attachments WHERE id=OLD.attachment_id; END)"})
    {
        DO_OR_RETURN(db.execute(sql));
    }
    return {};
}

E<std::vector<std::pair<int64_t, int64_t>>>
screenshotCleanup(UploadDatabase& db)
{
    ASSIGN_OR_RETURN(
        auto rows,
        (db.eval<int64_t, int64_t>(
            "SELECT user_id,attachment_id FROM GameScreenshotCleanup")));
    std::vector<std::pair<int64_t, int64_t>> result;
    for(auto [user, id] : rows)
    {
        result.emplace_back(user, id);
    }
    return result;
}

E<std::vector<GameScreenshot>> listScreenshots(UploadDatabase& db,
                                               int64_t game_id,
                                               const Configuration& config)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(R"(
SELECT s.id,s.attachment_id,s.added,a.user_id,
 s.thumbnail_codec,s.thumbnail_size,s.thumbnail_quality,
 s.preview_codec,s.preview_size,s.preview_quality,a.added,a.original_name,
 f.hash,f.mime_type,f.byte_size,f.created
 FROM GameScreenshots s JOIN Attachments a ON a.id=s.attachment_id
 JOIN AttachmentFiles f ON f.hash=a.file_hash
 WHERE s.game_id=? ORDER BY s.added DESC,s.id DESC)"));
    DO_OR_RETURN(sql.bind(game_id));
    ASSIGN_OR_RETURN(auto rows,
                     (db.eval<int64_t, int64_t, int64_t, int64_t, int, int, int,
                              int, int, int, int64_t, std::string, std::string,
                              std::string, int64_t, int64_t>(std::move(sql))));
    std::vector<GameScreenshot> result;
    for(auto& [id, attachment_id, added, user, tc, ts, tq, pc, ps, pq,
               attachment_added, name, hash, mime, size, created] : rows)
    {
        if(tc < 0 || tc > 3 || pc < 0 || pc > 3)
        {
            return std::unexpected(
                runtimeError("Invalid stored screenshot codec"));
        }
        GameScreenshot shot;
        shot.id = id;
        shot.game_id = game_id;
        shot.added = added;
        shot.thumbnail_settings = {static_cast<ScreenshotCodec>(tc), tq, ts};
        shot.preview_settings = {static_cast<ScreenshotCodec>(pc), pq, ps};
        shot.attachment = {attachment_id,
                           user,
                           attachment_added,
                           name,
                           {hash, mime, size, created}};
        auto historical = config;
        historical.attachment_image_dimension = 65536;
        ASSIGN_OR_RETURN(auto thumbnail_recipe,
                         imageRecipe(screenshotOptions(shot.thumbnail_settings),
                                     historical));
        ASSIGN_OR_RETURN(
            auto preview_recipe,
            imageRecipe(screenshotOptions(shot.preview_settings), historical));
        ASSIGN_OR_RETURN(shot.thumbnail,
                         variant(db, hash, thumbnail_recipe, "thumbnail"));
        ASSIGN_OR_RETURN(shot.preview,
                         variant(db, hash, preview_recipe, "preview"));
        result.push_back(std::move(shot));
    }
    return result;
}

E<bool> screenshotDuplicate(UploadDatabase& db, int64_t game_id,
                            int64_t attachment_id)
{
    ASSIGN_OR_RETURN(
        auto pending,
        db.statementFromStr("SELECT attachment_id FROM GameScreenshotCleanup "
                            "WHERE attachment_id=?"));
    DO_OR_RETURN(pending.bind(attachment_id));
    ASSIGN_OR_RETURN(auto queue, db.eval<int64_t>(std::move(pending)));
    if(!queue.empty())
    {
        return std::unexpected(
            httpError(409, "Attachment is awaiting cleanup; retry later"));
    }
    ASSIGN_OR_RETURN(
        auto sql,
        db.statementFromStr(
            "SELECT game_id FROM GameScreenshots WHERE attachment_id=?"));
    DO_OR_RETURN(sql.bind(attachment_id));
    ASSIGN_OR_RETURN(auto rows, db.eval<int64_t>(std::move(sql)));
    if(rows.empty())
    {
        return false;
    }
    if(std::get<0>(rows[0]) != game_id)
    {
        return std::unexpected(httpError(
            409, "Attachment is already a screenshot for another game"));
    }
    return true;
}

E<bool> associateScreenshot(UploadDatabase& db, const Configuration& config,
                            int64_t user, int64_t game_id,
                            const Attachment& attachment,
                            bool require_duplicate)
{
    ASSIGN_OR_RETURN(
        auto sql,
        db.statementFromStr("SELECT user_id FROM GameTracking WHERE id=?"));
    DO_OR_RETURN(sql.bind(game_id));
    ASSIGN_OR_RETURN(auto games, db.eval<int64_t>(std::move(sql)));
    if(games.empty())
    {
        return std::unexpected(httpError(404, "Game not found"));
    }
    if(std::get<0>(games[0]) != user || attachment.user_id != user)
    {
        return std::unexpected(httpError(403, "Screenshot owner mismatch"));
    }
    ASSIGN_OR_RETURN(auto duplicate,
                     screenshotDuplicate(db, game_id, attachment.id));
    if(duplicate)
    {
        return true;
    }
    if(require_duplicate)
    {
        return std::unexpected(
            httpError(409, "Screenshot changed; retry upload"));
    }
    DO_OR_RETURN(validateScreenshotSettings(config));
    for(auto settings :
        {config.screenshot_thumbnail, config.screenshot_preview})
    {
        ASSIGN_OR_RETURN(auto recipe,
                         imageRecipe(screenshotOptions(settings), config));
        DO_OR_RETURN(variant(db, attachment.file.hash, recipe, "variant"));
    }
    auto thumbnail = config.screenshot_thumbnail;
    auto preview = config.screenshot_preview;
    DO_OR_RETURN(execute(
        db,
        "INSERT INTO GameScreenshots(game_id,attachment_id,thumbnail_codec,"
        "thumbnail_size,thumbnail_quality,preview_codec,preview_size,"
        "preview_quality,added) VALUES(?,?,?,?,?,?,?,?,?)",
        game_id, attachment.id, static_cast<int>(thumbnail.codec),
        thumbnail.long_edge, thumbnail.quality, static_cast<int>(preview.codec),
        preview.long_edge, preview.quality,
        static_cast<int64_t>(time(nullptr))));
    return false;
}

E<void> removeScreenshot(UploadDatabase& db, int64_t user, int64_t game_id,
                         int64_t id)
{
    ASSIGN_OR_RETURN(auto sql, db.statementFromStr(R"(
SELECT s.id FROM GameScreenshots s JOIN GameTracking g ON g.id=s.game_id
 WHERE s.id=? AND s.game_id=? AND g.user_id=?)"));
    DO_OR_RETURN(sql.bind(id, game_id, user));
    ASSIGN_OR_RETURN(auto rows, db.eval<int64_t>(std::move(sql)));
    if(rows.empty())
    {
        return std::unexpected(httpError(404, "Screenshot not found"));
    }
    return execute(db, "DELETE FROM GameScreenshots WHERE id=?", id);
}
