#include "StrategyFactory.h"

#include "FixedWindowCounter.h"
#include "LeakyBucket.h"
#include "SlidingWindowCounter.h"
#include "SlidingWindowLog.h"
#include "TokenBucket.h"

#include <sstream>

namespace {
bool readParam(const std::map<std::string, double>& params,
               const std::string& name,
               double& value,
               std::string* error) {
    auto it = params.find(name);
    if (it == params.end()) {
        if (error) *error = "missing parameter: " + name;
        return false;
    }
    value = it->second;
    return true;
}

int asInt(double value) {
    return static_cast<int>(value);
}
}

std::unique_ptr<RateLimitStrategy> createStrategy(
    const std::string& algorithm,
    const std::map<std::string, double>& params,
    std::string* error) {
    std::string normalized = normalizeAlgorithmName(algorithm);

    double first = 0.0;
    double second = 0.0;
    if (normalized == "TokenBucket") {
        if (!readParam(params, "capacity", first, error)) return nullptr;
        if (!readParam(params, "refill", second, error)) return nullptr;
        return std::make_unique<TokenBucket>(first, second);
    }
    if (normalized == "FixedWindowCounter") {
        if (!readParam(params, "limit", first, error)) return nullptr;
        if (!readParam(params, "window", second, error)) return nullptr;
        return std::make_unique<FixedWindowCounter>(asInt(first), second);
    }
    if (normalized == "SlidingWindowLog") {
        if (!readParam(params, "limit", first, error)) return nullptr;
        if (!readParam(params, "window", second, error)) return nullptr;
        return std::make_unique<SlidingWindowLog>(asInt(first), second);
    }
    if (normalized == "SlidingWindowCounter") {
        if (!readParam(params, "limit", first, error)) return nullptr;
        if (!readParam(params, "window", second, error)) return nullptr;
        return std::make_unique<SlidingWindowCounter>(asInt(first), second);
    }
    if (normalized == "LeakyBucket") {
        if (!readParam(params, "capacity", first, error)) return nullptr;
        if (!readParam(params, "leakRate", second, error)) return nullptr;
        return std::make_unique<LeakyBucket>(first, second);
    }

    if (error) *error = "unsupported algorithm: " + algorithm;
    return nullptr;
}

std::string buildPolicyKey(const std::string& tenantId, const Policy& policy) {
    std::ostringstream oss;
    oss << tenantId << "|" << normalizeAlgorithmName(policy.algorithm)
        << "|" << policy.paramsString();
    return oss.str();
}
