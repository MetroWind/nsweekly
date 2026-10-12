#pragma once
#include "attachment/attachment_service.hpp"
#include "games/game_data.hpp"
#include "games/game_screenshot.hpp"

class GameScreenshotService;
// Holds screenshot coordination through execution and response mapping.
class ScreenshotWriter
{
public:
    // Transfers the exclusive upload lifecycle without copying file state.
    ScreenshotWriter(ScreenshotWriter&&) = default;
    ScreenshotWriter& operator=(ScreenshotWriter&&) = delete;
    // Streams a bounded chunk to the attachment service.
    E<void> append(std::string_view bytes);
    // Publishes atomically and resolves the committed screenshot response.
    E<GameScreenshot> finish();

private:
    friend class GameScreenshotService;
    ScreenshotWriter(GameScreenshotService& owner, int64_t game_id,
                     std::unique_lock<std::mutex> coordinator,
                     std::unique_ptr<bool> duplicate,
                     std::unique_ptr<UploadExecution> upload)
        : service(owner), game(game_id), lock(std::move(coordinator)),
          duplicate(std::move(duplicate)), writer(std::move(upload))
    {
    }
    GameScreenshotService& service;
    int64_t game;
    std::unique_lock<std::mutex> lock;
    // Stable consumer-owned output state outlives the task even after a move.
    std::unique_ptr<bool> duplicate;
    std::unique_ptr<UploadExecution> writer;
};

// Coordinates screenshot lifetimes with game deletion and durable cleanup.
class GameScreenshotService
{
public:
    // Borrows application-owned attachment and game storage dependencies.
    GameScreenshotService(AttachmentService& storage, GameDataInterface& data,
                          const Configuration& settings)
        : attachments(storage), games(data), config(settings)
    {
    }
    // Validates coders, creates schema and drains crash recovery work.
    E<void> initialize();
    // Verifies the game before returning a streaming upload lifecycle.
    E<ScreenshotWriter> beginUpload(const std::string& owner, int64_t user,
                                    int64_t game, std::string_view filename);
    // Returns newest-first public metadata; never encodes during a read.
    E<std::vector<GameScreenshot>> list(int64_t game);
    // Deletes a screenshot association and retries its attachment release.
    E<void> remove(int64_t user, int64_t game, int64_t id);
    // Deletes a game and atomically cascades screenshots to durable cleanup.
    E<bool> deleteGame(const std::string& user, int64_t game);
    // Retries queued releases while excluding active screenshot uploads.
    E<void> cleanup();

private:
    E<void> drain();
    void retry();
    AttachmentService& attachments;
    GameDataInterface& games;
    const Configuration& config;
    std::mutex coordinator;
};
