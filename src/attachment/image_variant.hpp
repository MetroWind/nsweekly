#pragma once
#include "attachment/attachment_files.hpp"
#include "config.hpp"

// Validates finite application limits before startup or conversion.
E<void> validateImageSettings(const Configuration& config);
// Initializes Magick++ and process-wide cache limits exactly once.
E<void> initializeImageConverter();
// Validates every option and creates the fixed-order v1 recipe.
E<std::string> imageRecipe(const ImageVariantOptions& options,
                           const Configuration& config);
// Decodes a pinned source descriptor and streams encoded bytes into staging.
E<std::pair<int, int>> convertImage(int descriptor, const std::string& mime,
                                    const ImageVariantOptions& options,
                                    const Configuration& config,
                                    AttachmentStaging& output);
