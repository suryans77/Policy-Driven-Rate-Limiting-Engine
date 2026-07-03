#include "SlidingWindowLog.h"
#include <sstream>
#include <iomanip>

using namespace std::chrono;

SlidingWindowLog::SlidingWindowLog(int limit, double windowSecs)
    : limit_(limit), windowSecs_(windowSecs)
{}

void SlidingWindowLog::evictOldEntries(const steady_clock::time_point& now) {
    // Window start for this evaluation
    auto cutoff = now - duration<double>(windowSecs_);
    // deque is time-ordered: front is oldest. Pop until front is within window.
    while (!log_.empty() && log_.front() <= cutoff) {
        log_.pop_front();
    }
}

bool SlidingWindowLog::allowRequest() {
    auto now = steady_clock::now();
    evictOldEntries(now);

    if (static_cast<int>(log_.size()) < limit_) {
        log_.push_back(now);
        return true;
    }
    return false;
}

std::string SlidingWindowLog::getState() const {
    auto now     = steady_clock::now();
    auto cutoff  = now - duration<double>(windowSecs_);

    // Count without mutating (const method — can't call evict)
    int active = 0;
    for (const auto& tp : log_) {
        if (tp > cutoff) ++active;
    }

    std::ostringstream oss;
    oss << "log_size=" << active << "/" << limit_
        << " window=" << windowSecs_ << "s";
    return oss.str();
}

void SlidingWindowLog::reset() {
    log_.clear();
}

std::string SlidingWindowLog::algorithmName() const {
    return "SlidingWindowLog";
}
