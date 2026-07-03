#include "SlidingWindowCounter.h"
#include <sstream>
#include <iomanip>

using namespace std::chrono;

SlidingWindowCounter::SlidingWindowCounter(int limit, double windowSecs)
    : limit_(limit),
      windowSecs_(windowSecs),
      prevCount_(0),
      currCount_(0),
      windowStart_(steady_clock::now())
{}

void SlidingWindowCounter::maybeAdvanceWindow(const steady_clock::time_point& now) {
    double elapsed = duration<double>(now - windowStart_).count();

    if (elapsed >= windowSecs_) {
        // How many complete windows have passed?
        int windowsPassed = static_cast<int>(elapsed / windowSecs_);

        if (windowsPassed == 1) {
            // Normal case: exactly one window rolled over
            prevCount_ = currCount_;
        } else {
            // Multiple windows passed with no traffic — prev is effectively 0
            prevCount_ = 0;
        }
        currCount_   = 0;
        // Advance windowStart_ by exactly windowsPassed windows
        windowStart_ += duration_cast<steady_clock::duration>(
            duration<double>(windowsPassed * windowSecs_));
    }
}

bool SlidingWindowCounter::allowRequest() {
    auto now = steady_clock::now();
    maybeAdvanceWindow(now);

    // f = fraction elapsed through current window (0.0 to 1.0)
    double elapsed = duration<double>(now - windowStart_).count();
    double f       = elapsed / windowSecs_;

    double estimate = prevCount_ * (1.0 - f) + currCount_;

    if (estimate < static_cast<double>(limit_)) {
        ++currCount_;
        return true;
    }
    return false;
}

std::string SlidingWindowCounter::getState() const {
    auto now     = steady_clock::now();
    double elapsed = duration<double>(now - windowStart_).count();
    double f       = elapsed / windowSecs_;
    double estimate = prevCount_ * (1.0 - f) + currCount_;

    std::ostringstream oss;
    oss << "prev=" << prevCount_
        << " curr=" << currCount_
        << " f=" << std::fixed << std::setprecision(2) << f
        << " estimate=" << std::setprecision(2) << estimate
        << "/" << limit_
        << " window=" << windowSecs_ << "s";
    return oss.str();
}

void SlidingWindowCounter::reset() {
    prevCount_   = 0;
    currCount_   = 0;
    windowStart_ = steady_clock::now();
}

std::string SlidingWindowCounter::algorithmName() const {
    return "SlidingWindowCounter";
}
