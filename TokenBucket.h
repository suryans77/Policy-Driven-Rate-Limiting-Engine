#pragma once
#include "RateLimitStrategy.h"
#include <chrono>

class TokenBucket : public RateLimitStrategy {
public:
    TokenBucket(double capacity, double refillRatePerSec);

    bool allowRequest() override;
    std::string getState() const override;
    void reset() override;
    std::string algorithmName() const override;

private:
    double capacity_;
    double refillRatePerSec_;
    double tokens_;
    std::chrono::steady_clock::time_point lastRefill_;

    void refill();
};