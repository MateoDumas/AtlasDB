#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace atlas {

// Database-wide reader/writer lock. Exclusive ownership belongs to a session
// rather than a thread.
//
// A session holds the exclusive lock for the whole duration of an explicit
// transaction (BEGIN..COMMIT) or for a single autocommit write/DDL statement.
// Autocommit read-only statements take the shared lock, so they run in
// parallel with each other but never alongside a writer. This yields
// serializable isolation. Waiting writers block new readers, so a steady
// stream of SELECTs cannot starve a writer.
class TransactionLock {
public:
    // Returns false on timeout. Re-acquiring by the current owner is a no-op.
    bool acquire(uint64_t session_id, std::chrono::milliseconds timeout);
    void release(uint64_t session_id);
    bool held_by(uint64_t session_id) const;

    bool acquire_shared(std::chrono::milliseconds timeout);
    void release_shared();
    size_t readers() const;

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    uint64_t owner_ = 0;
    size_t readers_ = 0;
    size_t waiting_writers_ = 0;
};

}  // namespace atlas
