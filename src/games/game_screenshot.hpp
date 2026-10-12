#pragma once
#include "attachment/attachment.hpp"
#include "config.hpp"

// Public gallery record; compression settings remain fixed after publication.
struct GameScreenshot
{
    // Stable screenshot and parent game identities and creation time.
    int64_t id = 0, game_id = 0, added = 0;
    // Owner-scoped original reference, never used as a displayed image.
    Attachment attachment;
    // Persisted recipes and resolved compressed outputs.
    ScreenshotSettings thumbnail_settings, preview_settings;
    ImageVariant thumbnail, preview;
    // Retry of an already published association.
    bool already_uploaded = false;
};

// Converts typed settings to the shared attachment recipe input.
ImageVariantOptions screenshotOptions(const ScreenshotSettings& settings);

// Checks the configured screenshot codecs using the generic encoder probe.
E<void> validateScreenshotEncoders(const Configuration& config);
