#pragma once
#include "RateLimitStrategy.h"
#include <chrono>
#include <string>

// SlidingWindowCounter
//
// State  : prevCount (int), currCount (int), windowStart (time_point),
//          windowSize (duration).
// Decision: compute how far into the current window we are as a fraction f
//            (0.0 = just started, 1.0 = about to flip).
//            Weighted estimate = prevCount * (1 - f) + currCount.
//            Allow if estimate < limit, then increment currCount.
//            On window flip: prevCount = currCount, currCount = 0, windowStart advances.
//
// Intuition: the previous window's traffic gets less weight the further into
// the current window you are. At f=0.25 (25% through new window), you still
// carry 75% of the previous window's count as an approximation of the true
// sliding load.
//
// The approximation: assumes traffic in the previous window was uniformly
// distributed. If it was bursty (all 100 reqs in the last 10ms of the window),
// the weighted estimate underestimates true recent load. Max error = limit * f.
//
// This is what Redis uses in its built-in rate limiter for exactly this tradeoff:
// O(1) time, O(1) space, near-accurate, no memory blowup.
//
// Complexity: O(1) time, O(1) space.

class SlidingWindowCounter : public RateLimitStrategy {
public:
    // limit      : max requests per window
    // windowSecs : window duration in seconds
    SlidingWindowCounter(int limit, double windowSecs);

    bool allowRequest() override;
    std::string getState() const override;
    void reset() override;
    std::string algorithmName() const override;

private:
    void maybeAdvanceWindow(const std::chrono::steady_clock::time_point& now);

    int    limit_;
    double windowSecs_;
    int    prevCount_;
    int    currCount_;
    std::chrono::steady_clock::time_point windowStart_;
};
