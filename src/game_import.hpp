#pragma once
#include <ostream>
#include "config.hpp"
#include "game_data.hpp"

// Validates all CSV rows before inserts; reports duplicates and partial saves.
int importGamesCsv(const std::string &text, const std::string &user,
                   GameDataInterface &games, std::ostream &output);
// Runs import startup without authentication, templates, or an HTTP server.
int runGameImport(const Configuration &config, const std::string &file,
                  const std::string &user, std::ostream &output);
