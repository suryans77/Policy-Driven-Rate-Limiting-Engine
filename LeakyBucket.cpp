#include "LeakyBucket.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

using namespace std::chrono;

LeakyBucket::LeakyBucket(double capacity, double leakRate)
    : capacity_(capacity),
      leakRate_(leakRate),
      queue_(0.0),
      lastLeak_(steady_clock::now())
{}

void LeakyBucket::leak(const steady_clock::time_point& now) {
    double elapsed = duration<double>(now - lastLeak_).count();
    queue_ = std::max(0.0, queue_ - elapsed * leakRate_);
    lastLeak_ = now;
}

bool LeakyBucket::allowRequest() {
    auto now = steady_clock::now();
    leak(now);

    if (queue_ + 1.0 <= capacity_) {
        queue_ += 1.0;
        return true;
    }
    return false;
}

std::string LeakyBucket::getState() const {
    // Compute display-only leak without mutating state (const method)
    auto now     = steady_clock::now();
    double elapsed = duration<double>(now - lastLeak_).count();
    double displayQueue = std::max(0.0, queue_ - elapsed * leakRate_);

    std::ostringstream oss;
    oss << "queue=" << std::fixed << std::setprecision(2) << displayQueue
        << "/" << capacity_
        << " leak=" << leakRate_ << "/s";
    return oss.str();
}

void LeakyBucket::reset() {
    queue_    = 0.0;
    lastLeak_ = steady_clock::now();
}

std::string LeakyBucket::algorithmName() const {
    return "LeakyBucket";
}
