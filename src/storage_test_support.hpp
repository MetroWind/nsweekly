#pragma once
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

// Isolates multi-connection storage tests from the user's live database.
class TemporaryDatabase
{
public:
    // Allocates a unique temporary directory for the SQLite file.
    TemporaryDatabase()
    {
        std::array<char, 32> pattern{};
        std::string prefix = "/tmp/nsweekly_test_XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        if(auto created = mkdtemp(pattern.data()))
        {
            directory = created;
        }
    }
    // Cleans up only this test's database and associated files.
    ~TemporaryDatabase()
    {
        if(!directory.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }
    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;
    // Returns the unique file path, or an empty string on allocation failure.
    std::string path() const
    {
        return directory.empty() ? "" : directory + "/data.db";
    }
private:
    std::string directory;
};
