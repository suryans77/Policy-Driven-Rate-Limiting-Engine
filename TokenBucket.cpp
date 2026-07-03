#include "TokenBucket.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

using namespace std::chrono;

TokenBucket::TokenBucket(double capacity, double refillRatePerSec)
    : capacity_(capacity),
      refillRatePerSec_(refillRatePerSec),
      tokens_(capacity),              // start full
      lastRefill_(steady_clock::now())
{}

void TokenBucket::refill() {
    auto now = steady_clock::now();
    double elapsed = duration<double>(now - lastRefill_).count(); // seconds
    double newTokens = elapsed * refillRatePerSec_;
    tokens_ = std::min(capacity_, tokens_ + newTokens);
    lastRefill_ = now;
}

bool TokenBucket::allowRequest() {
    refill();
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;
    }
    return false;
}

std::string TokenBucket::getState() const {
    std::ostringstream oss;
    oss << "tokens=" << std::fixed << std::setprecision(2) << tokens_
        << "/" << capacity_
        << " refill=" << refillRatePerSec_ << "/s";
    return oss.str();
}

void TokenBucket::reset() {
    tokens_ = capacity_;
    lastRefill_ = steady_clock::now();
}

std::string TokenBucket::algorithmName() const {
    return "TokenBucket";
}