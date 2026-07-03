#include "Policy.h"

#include <iomanip>
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
