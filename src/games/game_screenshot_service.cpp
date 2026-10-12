#include "games/game_screenshot_service.hpp"
#include "games/game_screenshot_data.hpp"
#include "attachment/image_variant.hpp"
#include <spdlog/spdlog.h>

namespace
{
// Games policy only; all staging, conversion mechanics and commits are generic.
class ScreenshotUpload : public Upload
{
public:
    // Captures games dependencies and consumer-owned duplicate output.
    ScreenshotUpload(UploadInput input, const std::string& owner, int64_t game,
                     GameDataInterface& games, AttachmentService& attachments,
                     const Configuration& config, bool& already_uploaded)
        : Upload(std::move(input)), owner(owner), game(game), games(games),
          attachments(attachments), config(config),
          already_uploaded(already_uploaded)
    {
    }
    // Checks the consumer's owner-scoped game before any body bytes arrive.
    E<void> beginUpload(UploadStart& context) override
    {
        user = context.input.user_id;
        ASSIGN_OR_RETURN(auto record, games.getGame(owner, game));
        if(!record)
        {
            return std::unexpected(httpError(404, "Game not found"));
        }
        return attachments.readMetadata(
            [&](UploadDatabase& db) -> E<void>
            {
                ASSIGN_OR_RETURN(
                    auto sql,
                    db.statementFromStr(
                        "SELECT user_id FROM GameTracking WHERE id=?"));
                DO_OR_RETURN(sql.bind(game));
                ASSIGN_OR_RETURN(auto rows, db.eval<int64_t>(std::move(sql)));
                if(rows.empty())
                {
                    return std::unexpected(httpError(404, "Game not found"));
                }
                if(std::get<0>(rows[0]) != user)
                {
                    return std::unexpected(
                        httpError(403, "Screenshot owner mismatch"));
                }
                return {};
            });
    }
    // Selects screenshot recipes and skips advisory same-game duplicates.
    E<void> process(UploadProcessing& context) override
    {
        if(context.existingAttachment())
        {
            DO_OR_RETURN(context.readMetadata(
                [&](UploadDatabase& db) -> E<void>
                {
                    ASSIGN_OR_RETURN(
                        skipped_duplicate,
                        screenshotDuplicate(db, game,
                                            context.existingAttachment()->id));
                    return {};
                }));
            if(skipped_duplicate)
            {
                return {};
            }
        }
        DO_OR_RETURN(context.prepareVariant(
            screenshotOptions(config.screenshot_thumbnail)));
        return context.prepareVariant(
            screenshotOptions(config.screenshot_preview));
    }
    // Associates through the exact connection already in the publication tx.
    E<void> endUpload(UploadPublication& context) override
    {
        ASSIGN_OR_RETURN(already_uploaded,
                         associateScreenshot(context.database, config, user,
                                             game, context.attachment.record,
                                             skipped_duplicate));
        return {};
    }

private:
    std::string owner;
    int64_t game, user = 0;
    GameDataInterface& games;
    AttachmentService& attachments;
    const Configuration& config;
    bool& already_uploaded;
    bool skipped_duplicate = false;
};
} // namespace

E<void> GameScreenshotService::initialize()
{
    DO_OR_RETURN(validateScreenshotSettings(config));
    DO_OR_RETURN(validateScreenshotEncoders(config));
    DO_OR_RETURN(attachments.writeMetadata(initializeScreenshotSchema));
    return cleanup();
}

E<ScreenshotWriter>
GameScreenshotService::beginUpload(const std::string& owner, int64_t user,
                                   int64_t game, std::string_view filename)
{
    std::unique_lock lock(coordinator);
    retry();
    auto duplicate = std::make_unique<bool>(false);
    auto task = std::make_unique<ScreenshotUpload>(
        UploadInput{user, std::string(filename)}, owner, game, games,
        attachments, config, *duplicate);
    ASSIGN_OR_RETURN(auto execution, attachments.startUpload(std::move(task)));
    return ScreenshotWriter(*this, game, std::move(lock), std::move(duplicate),
                            std::move(execution));
}

E<void> ScreenshotWriter::append(std::string_view bytes)
{
    if(!writer || !lock.owns_lock())
    {
        return std::unexpected(runtimeError("Screenshot upload closed"));
    }
    auto result = writer->append(bytes);
    if(!result)
    {
        writer.reset();
        lock.unlock();
    }
    return result;
}

E<GameScreenshot> ScreenshotWriter::finish()
{
    if(!writer || !lock.owns_lock())
    {
        return std::unexpected(runtimeError("Screenshot upload closed"));
    }
    // Keep games deletion excluded until committed response metadata is read.
    auto completion_lock = std::move(lock);
    ASSIGN_OR_RETURN(auto upload, writer->finish());
    ASSIGN_OR_RETURN(auto shots, service.list(game));
    for(auto& shot : shots)
    {
        if(shot.attachment.id == upload.record.id)
        {
            shot.already_uploaded = *duplicate;
            return shot;
        }
    }
    return std::unexpected(httpError(404, "Screenshot no longer exists"));
}

E<std::vector<GameScreenshot>> GameScreenshotService::list(int64_t game)
{
    std::vector<GameScreenshot> result;
    DO_OR_RETURN(attachments.readMetadata(
        [&](UploadDatabase& db) -> E<void>
        {
            ASSIGN_OR_RETURN(result, listScreenshots(db, game, config));
            return {};
        }));
    return result;
}

E<void> GameScreenshotService::remove(int64_t user, int64_t game, int64_t id)
{
    std::lock_guard lock(coordinator);
    DO_OR_RETURN(attachments.writeMetadata(
        [&](UploadDatabase& db)
        { return removeScreenshot(db, user, game, id); }));
    retry();
    return {};
}

E<bool> GameScreenshotService::deleteGame(const std::string& user, int64_t game)
{
    std::lock_guard lock(coordinator);
    ASSIGN_OR_RETURN(auto removed, games.deleteGame(user, game));
    retry();
    return removed;
}

E<void> GameScreenshotService::drain()
{
    std::vector<std::pair<int64_t, int64_t>> pending;
    DO_OR_RETURN(attachments.readMetadata(
        [&](UploadDatabase& db) -> E<void>
        {
            ASSIGN_OR_RETURN(pending, screenshotCleanup(db));
            return {};
        }));
    for(auto [user, id] : pending)
    {
        auto deleted = attachments.deleteAttachment(user, id);
        if(!deleted)
        {
            const auto* http = deleted.error().as<HTTPError>();
            if(!http || http->code != 404)
            {
                spdlog::error("Screenshot attachment cleanup: {}",
                              errorMsg(deleted.error()));
            }
        }
    }
    return {};
}

void GameScreenshotService::retry()
{
    auto result = drain();
    if(!result)
    {
        spdlog::error("Screenshot cleanup: {}", errorMsg(result.error()));
    }
}

E<void> GameScreenshotService::cleanup()
{
    std::lock_guard lock(coordinator);
    return drain();
}
