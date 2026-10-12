#pragma once
#include "attachment/upload.hpp"
#include "games/game_screenshot.hpp"

// Creates consumer tables inside the caller's metadata transaction.
E<void> initializeScreenshotSchema(UploadDatabase& db);
// Resolves committed gallery metadata without generating any variants.
E<std::vector<GameScreenshot>> listScreenshots(UploadDatabase& db,
                                               int64_t game_id,
                                               const Configuration& config);
// Checks association and deletion-queue conflicts on the supplied connection.
E<bool> screenshotDuplicate(UploadDatabase& db, int64_t game_id,
                            int64_t attachment_id);
// Writes the association without opening or committing a transaction.
E<bool> associateScreenshot(UploadDatabase& db, const Configuration& config,
                            int64_t user, int64_t game_id,
                            const Attachment& attachment,
                            bool require_duplicate);
// Deletes an owned association, leaving consumer-owned release work.
E<void> removeScreenshot(UploadDatabase& db, int64_t user, int64_t game_id,
                         int64_t id);
// Copies the consumer deletion queue for generic attachment deletion.
E<std::vector<std::pair<int64_t, int64_t>>>
screenshotCleanup(UploadDatabase& db);
