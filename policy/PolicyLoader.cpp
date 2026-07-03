#include "PolicyLoader.h"
#include "PolicyResolver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
std::string trim(const std::string& s) {
    std::size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;

    std::size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;

    return s.substr(first, last - first);
}

int leadingSpaces(const std::string& s) {
    int count = 0;
    while (count < static_cast<int>(s.size()) && s[count] == ' ') ++count;
    return count;
}

std::string removeComment(const std::string& s) {
    bool inQuote = false;
    char quote = '\0';
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if ((c == '"' || c == '\'') && (i == 0 || s[i - 1] != '\\')) {
            if (!inQuote) {
                inQuote = true;
                quote = c;
            } else if (quote == c) {
                inQuote = false;
            }
        }
        if (c == '#' && !inQuote) return s.substr(0, i);
    }
    return s;
}

std::string stripQuotes(const std::string& s) {
    std::string out = trim(s);
    if (out.size() >= 2) {
        char first = out.front();
        char last = out.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return out.substr(1, out.size() - 2);
        }
    }
    return out;
}

bool splitKeyValue(const std::string& line, std::string& key, std::string& value) {
    auto pos = line.find(':');
    if (pos == std::string::npos) return false;
    key = trim(line.substr(0, pos));
    value = stripQuotes(line.substr(pos + 1));
    return !key.empty();
}

bool parseInt(const std::string& value, int& out) {
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') return false;
    out = static_cast<int>(parsed);
    return true;
}

bool parseDouble(const std::string& value, double& out) {
    char* end = nullptr;
    double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') return false;
    out = parsed;
    return true;
}

std::string normalizeParamName(const std::string& key) {
    if (key == "refillRate") return "refill";
    if (key == "leak") return "leakRate";
    return key;
}

bool isParamKey(const std::string& key) {
    std::string normalized = normalizeParamName(key);
    return normalized == "capacity"
        || normalized == "refill"
        || normalized == "limit"
        || normalized == "window"
        || normalized == "leakRate";
}

std::string formatLineDouble(double value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

bool applyRootField(Policy& policy,
                    const std::string& key,
                    const std::string& value,
                    std::string& error) {
    if (key == "role") {
        error = "role matching is not supported in this build";
        return false;
    }
    if (key == "priority") {
        int priority = 0;
        if (!parseInt(value, priority)) {
            error = "invalid priority value: " + value;
            return false;
        }
        policy.priority = priority;
        return true;
    }
    if (key == "algorithm") {
        policy.algorithm = normalizeAlgorithmName(value);
        return true;
    }
    if (isParamKey(key)) {
        double number = 0.0;
        if (!parseDouble(value, number)) {
            error = "invalid numeric value for " + key + ": " + value;
            return false;
        }
        policy.params[normalizeParamName(key)] = number;
        return true;
    }

    error = "unknown policy field: " + key;
    return false;
}

bool applyMatchField(Policy& policy,
                     const std::string& key,
                     const std::string& value,
                     std::string& error) {
    if (key == "role") {
        error = "role matching is not supported in this build";
        return false;
    }
    if (key == "tenant" || key == "tenantId") {
        policy.hasTenant = true;
        policy.matchTenant = value;
        return true;
    }
    if (key == "endpoint") {
        policy.hasEndpoint = true;
        policy.matchEndpoint = value;
        return true;
    }

    error = "unknown match field: " + key;
    return false;
}

bool validatePolicy(const Policy& policy, std::string& error) {
    if (policy.algorithm.empty()) {
        error = "policy is missing algorithm";
        return false;
    }
    if (policy.algorithm != "TokenBucket"
        && policy.algorithm != "FixedWindowCounter"
        && policy.algorithm != "SlidingWindowLog"
        && policy.algorithm != "SlidingWindowCounter"
        && policy.algorithm != "LeakyBucket") {
        error = "unsupported algorithm: " + policy.algorithm;
        return false;
    }
    return true;
}
}

PolicyLoadResult PolicyLoader::loadFromFile(const std::string& path) {
    PolicyLoadResult result;
    std::ifstream in(path);
    if (!in) {
        result.error = "unable to open policy file: " + path;
        return result;
    }

    std::vector<Policy> policies;
    Policy current;
    bool inPolicy = false;
    bool inMatch = false;
    int matchIndent = -1;
    std::size_t order = 0;
    std::string line;
    int lineNo = 0;

    auto fail = [&](const std::string& message) {
        result.error = path + ":" + std::to_string(lineNo) + ": " + message;
        return result;
    };

    auto flush = [&]() -> bool {
        if (!inPolicy) return true;
        std::string error;
        if (!validatePolicy(current, error)) {
            result.error = path + ": " + error;
            return false;
        }
        policies.push_back(current);
        return true;
    };

    while (std::getline(in, line)) {
        ++lineNo;
        std::string uncommented = removeComment(line);
        std::string trimmed = trim(uncommented);
        if (trimmed.empty()) continue;

        int indent = leadingSpaces(uncommented);
        if (!trimmed.empty() && trimmed[0] == '-') {
            if (!flush()) return result;

            current = Policy();
            current.order = order++;
            inPolicy = true;
            inMatch = false;
            matchIndent = -1;

            trimmed = trim(trimmed.substr(1));
            if (trimmed.empty()) continue;

            std::string key, value;
            if (!splitKeyValue(trimmed, key, value)) return fail("expected key: value after '-'");
            std::string error;
            if (!applyRootField(current, key, value, error)) return fail(error);
            continue;
        }

        if (!inPolicy) return fail("expected '-' to start a policy");

        if (trimmed == "match:") {
            inMatch = true;
            matchIndent = indent;
            continue;
        }

        std::string key, value;
        if (!splitKeyValue(trimmed, key, value)) return fail("expected key: value");

        bool stillInMatch = inMatch && (matchIndent < 0 || indent > matchIndent);
        if (key == "algorithm" || key == "priority" || isParamKey(key)) {
            stillInMatch = false;
            inMatch = false;
        }

        std::string error;
        bool ok = stillInMatch
            ? applyMatchField(current, key, value, error)
            : applyRootField(current, key, value, error);
        if (!ok) return fail(error);
    }

    if (!flush()) return result;

    PolicyResolver resolver(policies);
    result.ok = true;
    result.policies = resolver.policies();
    return result;
}

bool PolicyLoader::saveToFile(const std::string& path,
                              const std::vector<Policy>& policies,
                              std::string* error) {
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "unable to write policy file: " + path;
        return false;
    }
    out << toYaml(policies);
    return true;
}

std::string PolicyLoader::toYaml(const std::vector<Policy>& policies) {
    std::ostringstream out;
    for (const auto& policy : policies) {
        out << "- priority: " << policy.priority << "\n";
        if (policy.hasTenant || policy.hasEndpoint) {
            out << "  match:\n";
            if (policy.hasTenant) out << "    tenant: \"" << policy.matchTenant << "\"\n";
            if (policy.hasEndpoint) out << "    endpoint: \"" << policy.matchEndpoint << "\"\n";
        }
        out << "  algorithm: " << policy.algorithm << "\n";
        for (const auto& param : policy.params) {
            std::string key = param.first == "refill" ? "refillRate" : param.first;
            out << "  " << key << ": " << formatLineDouble(param.second) << "\n";
        }
    }
    return out.str();
}
