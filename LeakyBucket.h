#pragma once
#include "RateLimitStrategy.h"
#include <chrono>
#include <string>

// LeakyBucket
//
// Mental model: a bucket with a hole in the bottom. Water (requests) pours in
// at an arbitrary rate. It leaks out at a constant rate. If the bucket overflows,
// the request is denied.
//
// State  : double queue_ (current water level, i.e. queued requests),
//          double capacity_ (max bucket size),
//          double leakRate_ (requests drained per second),
//          time_point lastLeak_.
// Decision: compute elapsed time, drain queue_ by elapsed * leakRate_ (floor at 0),
//            then allow if queue_ + 1 <= capacity_, and increment queue_.
//
// THE KEY CONTRAST WITH TOKEN BUCKET:
//   Token Bucket   → burst is allowed THROUGH immediately (output is bursty).
//                    Smooths input, output rate can spike.
//   Leaky Bucket   → burst is ABSORBED into the queue, drained at constant rate.
//                    Output is always at most leakRate req/s, regardless of input.
//
// Use Leaky Bucket when the downstream system cannot handle any burst at all —
// e.g. a legacy database with strict connection limits, or a third-party API
// with per-second hard limits that cause errors (not just backpressure).
//
// Complexity: O(1) time, O(1) space.

class LeakyBucket : public RateLimitStrategy {
public:
    // capacity  : max queue depth (burst absorption ceiling)
    // leakRate  : requests drained per second (= sustained output rate)
    LeakyBucket(double capacity, double leakRate);

    bool allowRequest() override;
    std::string getState() const override;
    void reset() override;
    std::string algorithmName() const override;

private:
    void leak(const std::chrono::steady_clock::time_point& now);

    double capacity_;
    double leakRate_;
    double queue_;     // current water level
    std::chrono::steady_clock::time_point lastLeak_;
};
