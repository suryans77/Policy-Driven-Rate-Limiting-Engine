#include "Policy.h"

#include <iomanip>
#include <cmath>
#include <climits>
#include <set>
#include <sstream>

namespace {
std::string formatDouble(double value) {
    std::ostringstream oss;
    oss << std::setprecision(15) << value;
    std::string out = oss.str();

    if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
    }
    return out.empty() ? "0" : out;
}
}

bool Policy::matches(const Request& req) const {
    if (hasTenant && matchTenant != req.tenantId) return false;
    if (hasEndpoint && matchEndpoint != req.endpoint) return false;
    return true;
}

std::string Policy::paramsString() const {
    std::ostringstream oss;
    bool first = true;
    for (const auto& entry : params) {
        if (!first) oss << "|";
        oss << entry.first << "=" << formatDouble(entry.second);
        first = false;
    }
    return oss.str();
}

std::string normalizeAlgorithmName(const std::string& algorithm) {
    if (algorithm == "FixedWindow") return "FixedWindowCounter";
    if (algorithm == "SlidingWindow") return "SlidingWindowCounter";
    if (algorithm == "SlidingLog") return "SlidingWindowLog";
    return algorithm;
}

bool validatePolicyConfiguration(const Policy& policy, std::string& error) {
    const std::string algorithm = normalizeAlgorithmName(policy.algorithm);
    std::set<std::string> required;
    if (algorithm == "TokenBucket") required = {"capacity", "refill"};
    else if (algorithm == "FixedWindowCounter"
             || algorithm == "SlidingWindowLog"
             || algorithm == "SlidingWindowCounter") required = {"limit", "window"};
    else if (algorithm == "LeakyBucket") required = {"capacity", "leakRate"};
    else {
        error = algorithm.empty() ? "policy is missing algorithm"
                                  : "unsupported algorithm: " + algorithm;
        return false;
    }

    auto validMatchText = [&](const std::string& value, const std::string& name,
                              std::size_t maxLength) {
        if (value.empty() || value.size() > maxLength) {
            error = name + " must be non-empty and at most "
                + std::to_string(maxLength) + " bytes";
            return false;
        }
        for (unsigned char c : value) {
            if (c < 0x20 || c == 0x7F) {
                error = name + " contains a control character";
                return false;
            }
        }
        return true;
    };
    if (policy.hasTenant
        && !validMatchText(policy.matchTenant, "tenant", 256)) return false;
    if (policy.hasEndpoint) {
        if (!validMatchText(policy.matchEndpoint, "endpoint", 2048)) return false;
        if (policy.matchEndpoint.front() != '/') {
            error = "endpoint must be an absolute path";
            return false;
        }
    }

    for (const auto& name : required) {
        auto it = policy.params.find(name);
        if (it == policy.params.end()) {
            error = "missing parameter: " + name;
            return false;
        }
        if (!std::isfinite(it->second)) {
            error = "parameter must be finite: " + name;
            return false;
        }
    }
    for (const auto& param : policy.params) {
        if (required.count(param.first) == 0) {
            error = "unexpected parameter for " + algorithm + ": " + param.first;
            return false;
        }
    }

    auto positive = [&](const std::string& name) {
        if (policy.params.at(name) <= 0.0) {
            error = name + " must be greater than zero";
            return false;
        }
        return true;
    };
    if (required.count("capacity") && !positive("capacity")) return false;
    if (required.count("window") && !positive("window")) return false;
    if (required.count("refill") && policy.params.at("refill") < 0.0) {
        error = "refill must be non-negative";
        return false;
    }
    if (required.count("leakRate") && policy.params.at("leakRate") < 0.0) {
        error = "leakRate must be non-negative";
        return false;
    }
    if (required.count("limit")) {
        double limit = policy.params.at("limit");
        if (limit <= 0.0 || limit > static_cast<double>(INT_MAX)
            || std::floor(limit) != limit) {
            error = "limit must be a positive integer";
            return false;
        }
    }
    return true;
}
