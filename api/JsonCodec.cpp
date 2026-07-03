#include "JsonCodec.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {
std::string escapeJson(const std::string& value) {
    std::ostringstream out;
    for (char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c; break;
        }
    }
    return out.str();
}

std::string quote(const std::string& value) {
    return "\"" + escapeJson(value) + "\"";
}

bool findValueStart(const std::string& body, const std::string& key, std::size_t& pos) {
    std::string token = "\"" + key + "\"";
    pos = body.find(token);
    if (pos == std::string::npos) return false;

    pos = body.find(':', pos + token.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    return pos < body.size();
}

bool hasKey(const std::string& body, const std::string& key) {
    std::size_t pos = 0;
    return findValueStart(body, key, pos);
}

bool extractString(const std::string& body, const std::string& key, std::string& value) {
    std::size_t pos = 0;
    if (!findValueStart(body, key, pos) || body[pos] != '"') return false;

    ++pos;
    std::ostringstream out;
    bool escaped = false;
    for (; pos < body.size(); ++pos) {
        char c = body[pos];
        if (escaped) {
            switch (c) {
                case 'n': out << '\n'; break;
                case 'r': out << '\r'; break;
                case 't': out << '\t'; break;
                default: out << c; break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            value = out.str();
            return true;
        }
        out << c;
    }
    return false;
}

bool extractNumber(const std::string& body, const std::string& key, double& value) {
    std::size_t pos = 0;
    if (!findValueStart(body, key, pos)) return false;

    std::size_t end = pos;
    while (end < body.size()) {
        char c = body[end];
        if (!(std::isdigit(static_cast<unsigned char>(c))
              || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) break;
        ++end;
    }
    if (end == pos) return false;

    std::string number = body.substr(pos, end - pos);
    char* parseEnd = nullptr;
    double parsed = std::strtod(number.c_str(), &parseEnd);
    if (parseEnd == number.c_str() || *parseEnd != '\0') return false;

    value = parsed;
    return true;
}

void addNumberIfPresent(const std::string& body,
                        const std::string& jsonKey,
                        const std::string& paramKey,
                        Policy& policy) {
    double value = 0.0;
    if (extractNumber(body, jsonKey, value)) {
        policy.params[paramKey] = value;
    }
}

std::string numberToJson(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}
}

DecodeRequestResult JsonCodec::decodeEvaluateRequest(const std::string& body) {
    DecodeRequestResult result;
    if (hasKey(body, "role")) {
        result.error = "role is not supported";
        return result;
    }

    std::string tenant;
    if (!extractString(body, "tenant", tenant)) {
        extractString(body, "tenantId", tenant);
    }
    std::string endpoint;
    extractString(body, "endpoint", endpoint);

    if (tenant.empty()) {
        result.error = "missing tenant";
        return result;
    }
    if (endpoint.empty()) {
        result.error = "missing endpoint";
        return result;
    }

    result.ok = true;
    result.request = Request(tenant, endpoint);
    return result;
}

DecodePolicyResult JsonCodec::decodePolicy(const std::string& body) {
    DecodePolicyResult result;
    if (hasKey(body, "role")) {
        result.error = "role is not supported";
        return result;
    }

    Policy policy;
    double priority = 0.0;
    if (extractNumber(body, "priority", priority)) {
        policy.priority = static_cast<int>(priority);
    }

    std::string tenant;
    if (!extractString(body, "tenant", tenant)) {
        extractString(body, "tenantId", tenant);
    }
    if (!tenant.empty()) {
        policy.hasTenant = true;
        policy.matchTenant = tenant;
    }

    std::string endpoint;
    if (extractString(body, "endpoint", endpoint) && !endpoint.empty()) {
        policy.hasEndpoint = true;
        policy.matchEndpoint = endpoint;
    }

    std::string algorithm;
    if (!extractString(body, "algorithm", algorithm) || algorithm.empty()) {
        result.error = "missing algorithm";
        return result;
    }
    policy.algorithm = normalizeAlgorithmName(algorithm);

    addNumberIfPresent(body, "capacity", "capacity", policy);
    addNumberIfPresent(body, "refillRate", "refill", policy);
    addNumberIfPresent(body, "refill", "refill", policy);
    addNumberIfPresent(body, "limit", "limit", policy);
    addNumberIfPresent(body, "window", "window", policy);
    addNumberIfPresent(body, "leakRate", "leakRate", policy);
    addNumberIfPresent(body, "leak", "leakRate", policy);

    result.ok = true;
    result.policy = policy;
    return result;
}

std::string JsonCodec::encodeEvaluateResponse(bool allowed,
                                              const Request& request,
                                              const std::string& policyKey,
                                              const std::string& algorithm,
                                              const std::string& state) {
    std::ostringstream out;
    out << "{"
        << "\"allowed\":" << (allowed ? "true" : "false") << ","
        << "\"result\":\"" << (allowed ? "ALLOW" : "DENY") << "\","
        << "\"tenant\":" << quote(request.tenantId) << ","
        << "\"endpoint\":" << quote(request.endpoint) << ","
        << "\"policyKey\":" << quote(policyKey) << ","
        << "\"algorithm\":" << quote(algorithm) << ","
        << "\"state\":" << quote(state)
        << "}";
    return out.str();
}

std::string JsonCodec::encodePolicy(const Policy& policy) {
    std::ostringstream out;
    out << "{"
        << "\"priority\":" << policy.priority << ","
        << "\"match\":{";
    bool firstMatch = true;
    if (policy.hasTenant) {
        out << "\"tenant\":" << quote(policy.matchTenant);
        firstMatch = false;
    }
    if (policy.hasEndpoint) {
        if (!firstMatch) out << ",";
        out << "\"endpoint\":" << quote(policy.matchEndpoint);
    }
    out << "},"
        << "\"algorithm\":" << quote(policy.algorithm) << ","
        << "\"params\":{";

    bool firstParam = true;
    for (const auto& param : policy.params) {
        if (!firstParam) out << ",";
        out << quote(param.first) << ":" << numberToJson(param.second);
        firstParam = false;
    }
    out << "}}";
    return out.str();
}

std::string JsonCodec::encodePolicies(const std::vector<Policy>& policies) {
    std::ostringstream out;
    out << "{\"policies\":[";
    for (std::size_t i = 0; i < policies.size(); ++i) {
        if (i > 0) out << ",";
        out << encodePolicy(policies[i]);
    }
    out << "]}";
    return out.str();
}

std::string JsonCodec::encodeError(const std::string& message) {
    return "{\"error\":" + quote(message) + "}";
}
