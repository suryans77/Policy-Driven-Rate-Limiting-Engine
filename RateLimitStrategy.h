#pragma once
#include <string>

class RateLimitStrategy {
public:
    virtual bool allowRequest() = 0;
    virtual std::string getState() const = 0;
    virtual void reset() = 0;
    virtual std::string algorithmName() const = 0;
    virtual ~RateLimitStrategy() = default;
};