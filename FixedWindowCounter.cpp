#include "FixedWindowCounter.h"
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace std::chrono;

FixedWindowCounter::FixedWindowCounter(int limit, double windowSecs)
    : limit_(limit),
      windowSecs_(windowSecs),
      counter_(0),
      windowStart_(steady_clock::now())
{}

bool FixedWindowCounter::allowRequest() {
    auto now     = steady_clock::now();
    double elapsed = duration<double>(now - windowStart_).count();

    // Has the window expired? If so, open a new one.
    if (elapsed >= windowSecs_) {
        counter_     = 0;
        windowStart_ = now;
    }

    if (counter_ < limit_) {
        ++counter_;
        return true;
    }
    return false;
}

std::string FixedWindowCounter::getState() const {
    auto now     = steady_clock::now();
    double elapsed = duration<double>(now - windowStart_).count();
    double remaining = windowSecs_ - elapsed;
    if (remaining < 0) remaining = 0;

    std::ostringstream oss;
    oss << "count=" << counter_ << "/" << limit_
        << " window_remaining=" << std::fixed
        << std::setprecision(2) << remaining << "s"
        << " window=" << windowSecs_ << "s";
    return oss.str();
}

void FixedWindowCounter::reset() {
    counter_     = 0;
    windowStart_ = steady_clock::now();
}

std::string FixedWindowCounter::algorithmName() const {
    return "FixedWindowCounter";
}
