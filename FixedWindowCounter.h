#pragma once
#include "RateLimitStrategy.h"
#include <chrono>
#include <string>

// FixedWindowCounter
//
// State  : one int counter + the time_point when the current window opened.
// Decision: if now >= windowStart + windowSize, reset counter and record new
//            windowStart. Then allow if counter < limit, and increment.
//
// The boundary-burst exploit: a client can fire `limit` requests at t=0.99s
// (just before the window flips) and another `limit` at t=1.01s (just after).
// From the algorithm's view both windows are clean, but 2×limit requests
// crossed in ~20ms. This is the canonical reason sliding windows exist.
//
// Complexity: O(1) time, O(1) space.

class FixedWindowCounter : public RateLimitStrategy {
public:
    // limit      : max requests allowed per window
    // windowSecs : window duration in seconds (e.g. 1.0, 60.0)
    FixedWindowCounter(int limit, double windowSecs);

    bool allowRequest() override;
    std::string getState() const override;
    void reset() override;
    std::string algorithmName() const override;

private:
    int    limit_;
    double windowSecs_;
    int    counter_;
    std::chrono::steady_clock::time_point windowStart_;
};
