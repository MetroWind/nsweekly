#pragma once
#include <mutex>
#include "database.hpp"
#include "game.hpp"
#include "game_review.hpp"

// Owner-scoped game storage; no operation creates an account implicitly.
class GameDataInterface
{
  public:
    // Allows destruction through the storage interface.
    virtual ~GameDataInterface() = default;
    // Returns all games sorted by name and ID.
    virtual E<std::vector<GameRecord>> listGames(const std::string &user) = 0;
    // Returns a record only when it belongs to the named user.
    virtual E<std::optional<GameRecord>> getGame(const std::string &user,
                                                 int64_t id) = 0;
    // Creates one complete game, including platforms.
    virtual E<GameRecord> createGame(const std::string &user,
                                     const GameInput &input) = 0;
    // Replaces every field of an existing owner-scoped game.
    virtual E<GameRecord> updateGame(const std::string &user, int64_t id,
                                     const GameInput &input) = 0;
    // Lists review inputs owned by the named account.
    virtual E<std::vector<GameReviewRecord>> listReviews(
        const std::string &user) = 0;
    // Creates or replaces the single review for an owner-scoped game.
    virtual E<void> saveReview(const std::string &user, int64_t game_id,
                              const GameReviewInput &input) = 0;
    // Deletes only the review; a missing or foreign review returns false.
    virtual E<bool> deleteReview(const std::string &user, int64_t game_id) = 0;
    // Deletes the owner-scoped row and its platform assignments.
    virtual E<bool> deleteGame(const std::string &user, int64_t id) = 0;
};

// Owns a FULLMUTEX connection; serializes entire mutations, never reads.
class GameDataSqlite : public GameDataInterface
{
  public:
    // Takes ownership of a configured SQLite connection.
    explicit GameDataSqlite(std::unique_ptr<SQLite> connection)
        : db(std::move(connection))
    {
    }
    // Enables foreign keys and creates only games tables.
    E<void> initializeSchema();
    // Implements owner-scoped reads without taking the write lock.
    E<std::vector<GameRecord>> listGames(const std::string &user) override;
    // Retrieves a single owner-scoped row.
    E<std::optional<GameRecord>> getGame(const std::string &user,
                                         int64_t id) override;
    // Atomically inserts scalar values and platforms.
    E<GameRecord> createGame(const std::string &user,
                             const GameInput &input) override;
    // Atomically replaces scalar values and platforms.
    E<GameRecord> updateGame(const std::string &user, int64_t id,
                             const GameInput &input) override;
    // Removes a game and cascades platform deletion in one statement.
    E<bool> deleteGame(const std::string &user, int64_t id) override;

    // Implements owner-scoped review listing.
    E<std::vector<GameReviewRecord>> listReviews(
        const std::string &user) override;
    // Validates and atomically upserts a review without changing its game.
    E<void> saveReview(const std::string &user, int64_t game_id,
                       const GameReviewInput &input) override;
    // Removes a review without removing tracking data.
    E<bool> deleteReview(const std::string &user, int64_t game_id) override;

  private:
    E<GameRecord> save(const std::string &user, int64_t id,
                       const GameInput &input);
    std::unique_ptr<SQLite> db;
    std::mutex write_mutex;
};
