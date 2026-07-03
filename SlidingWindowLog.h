#pragma once
#include "RateLimitStrategy.h"
#include <chrono>
#include <deque>
#include <string>

// SlidingWindowLog
//
// State  : a deque<time_point> — one entry per accepted request.
// Decision: evict all entries older than (now - windowSize), then allow if
//            log.size() < limit. On allow, push_back(now).
//
// This is the only algorithm with perfect accuracy: the window truly slides
// per-request, so there is no boundary-burst exploit.
//
// The cost: O(n) space where n = requests in the current window.
// At 1000 req/s with a 60s window that's 60,000 time_point entries per tenant.
// Each steady_clock::time_point is typically 8 bytes → ~480 KB per busy tenant.
// At 10,000 tenants this is the algorithm that kills you.
//
// Eviction is O(n) worst case (all entries expire at once), O(1) amortized
// because each entry is pushed once and popped once.

class SlidingWindowLog : public RateLimitStrategy {
public:
    // limit      : max requests in any rolling window of windowSecs
    // windowSecs : window duration in seconds
    SlidingWindowLog(int limit, double windowSecs);

    bool allowRequest() override;
    std::string getState() const override;
    void reset() override;
    std::string algorithmName() const override;

private:
    void evictOldEntries(const std::chrono::steady_clock::time_point& now);

    int    limit_;
    double windowSecs_;
    std::deque<std::chrono::steady_clock::time_point> log_;
};
