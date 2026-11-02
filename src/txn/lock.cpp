#include "txn/lock.h"

namespace atlas {

bool TransactionLock::acquire(uint64_t session_id, std::chrono::milliseconds timeout) {
    std::unique_lock lk(m_);
    if (owner_ == session_id) return true;
    ++waiting_writers_;
    bool ok = cv_.wait_for(lk, timeout, [&] { return owner_ == 0 && readers_ == 0; });
    --waiting_writers_;
    if (!ok) {
        // Readers may have been held back only because we were waiting.
        cv_.notify_all();
        return false;
    }
    owner_ = session_id;
    return true;
}

void TransactionLock::release(uint64_t session_id) {
    {
        std::lock_guard lk(m_);
        if (owner_ != session_id) return;
        owner_ = 0;
    }
    cv_.notify_all();
}

bool TransactionLock::held_by(uint64_t session_id) const {
    std::lock_guard lk(m_);
    return owner_ == session_id;
}

bool TransactionLock::acquire_shared(std::chrono::milliseconds timeout) {
    std::unique_lock lk(m_);
    if (!cv_.wait_for(lk, timeout, [&] { return owner_ == 0 && waiting_writers_ == 0; })) return false;
    ++readers_;
    return true;
}

void TransactionLock::release_shared() {
    bool wake = false;
    {
        std::lock_guard lk(m_);
        if (readers_ > 0 && --readers_ == 0) wake = true;
    }
    if (wake) cv_.notify_all();
}

size_t TransactionLock::readers() const {
    std::lock_guard lk(m_);
    return readers_;
}

}  // namespace atlas
