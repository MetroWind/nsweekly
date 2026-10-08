#pragma once
#include <ostream>
#include "config.hpp"
#include "game_data.hpp"

// Validates all CSV rows before inserts; reports duplicates and partial saves.
int importGamesCsv(const std::string &text, const std::string &user,
                   GameDataInterface &games, std::ostream &output);
// Selects which spreadsheet table the standalone importer consumes.
enum class GameImportKind
{
    // Imports tracking fields and creates missing tracked games.
    TRACKER,
    // Imports review inputs linked to existing tracked games.
    REVIEWS
};
// Validates review CSV before owner-scoped insert-only migration.
int importReviewsCsv(const std::string &text, const std::string &user,
                     GameDataInterface &games, std::ostream &output);
// Runs import startup without authentication, templates, or an HTTP server.
int runGameImport(const Configuration &config, const std::string &file,
                  const std::string &user, std::ostream &output,
                  GameImportKind kind = GameImportKind::TRACKER);
