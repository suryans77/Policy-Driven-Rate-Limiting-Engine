#include "JsonCodec.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace {
struct JsonValue {
    enum Type { Null, Boolean, Number, String, Object, Array } type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        skipWhitespace();
        if (!parseValue(value, error, 0)) return false;
        skipWhitespace();
        if (pos_ != input_.size()) {
            error = "unexpected trailing JSON content";
            return false;
        }
        return true;
    }

private:
    bool parseValue(JsonValue& value, std::string& error, int depth) {
        if (depth > 32) return fail(error, "JSON nesting limit exceeded");
        skipWhitespace();
        if (pos_ >= input_.size()) return fail(error, "unexpected end of JSON");
        char c = input_[pos_];
        if (c == '{') return parseObject(value, error, depth + 1);
        if (c == '[') return parseArray(value, error, depth + 1);
        if (c == '"') {
            value.type = JsonValue::String;
            return parseString(value.string, error);
        }
        if (c == 't' && consumeLiteral("true")) {
            value.type = JsonValue::Boolean; value.boolean = true; return true;
        }
        if (c == 'f' && consumeLiteral("false")) {
            value.type = JsonValue::Boolean; value.boolean = false; return true;
        }
        if (c == 'n' && consumeLiteral("null")) {
            value.type = JsonValue::Null; return true;
        }
        return parseNumber(value, error);
    }

    bool parseObject(JsonValue& value, std::string& error, int depth) {
        value.type = JsonValue::Object;
        ++pos_;
        skipWhitespace();
        if (consume('}')) return true;
        while (true) {
            std::string key;
            if (!parseString(key, error)) return false;
            skipWhitespace();
            if (!consume(':')) return fail(error, "expected ':' after object key");
            JsonValue child;
            if (!parseValue(child, error, depth)) return false;
            if (!value.object.emplace(key, std::move(child)).second) {
                return fail(error, "duplicate JSON key: " + key);
            }
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return fail(error, "expected ',' or '}' in object");
            skipWhitespace();
        }
    }

    bool parseArray(JsonValue& value, std::string& error, int depth) {
        value.type = JsonValue::Array;
        ++pos_;
        skipWhitespace();
        if (consume(']')) return true;
        while (true) {
            JsonValue child;
            if (!parseValue(child, error, depth)) return false;
            value.array.push_back(std::move(child));
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return fail(error, "expected ',' or ']' in array");
            skipWhitespace();
        }
    }

    bool parseString(std::string& output, std::string& error) {
        if (!consume('"')) return fail(error, "expected JSON string");
        output.clear();
        while (pos_ < input_.size()) {
            unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail(error, "unescaped control character in string");
            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) return fail(error, "incomplete JSON escape");
            char escaped = input_[pos_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (pos_ >= input_.size()) return fail(error, "incomplete unicode escape");
                        char hex = input_[pos_++];
                        code <<= 4;
                        if (hex >= '0' && hex <= '9') code += hex - '0';
                        else if (hex >= 'a' && hex <= 'f') code += hex - 'a' + 10;
                        else if (hex >= 'A' && hex <= 'F') code += hex - 'A' + 10;
                        else return fail(error, "invalid unicode escape");
                    }
                    if (code >= 0xD800 && code <= 0xDFFF) {
                        return fail(error, "unicode surrogate pairs are not supported");
                    }
                    if (code <= 0x7F) output.push_back(static_cast<char>(code));
                    else if (code <= 0x7FF) {
                        output.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        output.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        output.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        output.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        output.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return fail(error, "invalid JSON escape");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    bool parseNumber(JsonValue& value, std::string& error) {
        std::size_t start = pos_;
        consume('-');
        if (pos_ >= input_.size()) return fail(error, "invalid JSON number");
        if (input_[pos_] == '0') ++pos_;
        else if (std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            while (pos_ < input_.size()
                   && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        } else return fail(error, "invalid JSON value");
        if (consume('.')) {
            std::size_t digits = pos_;
            while (pos_ < input_.size()
                   && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
            if (digits == pos_) return fail(error, "invalid JSON fraction");
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            std::size_t digits = pos_;
            while (pos_ < input_.size()
                   && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
            if (digits == pos_) return fail(error, "invalid JSON exponent");
        }
        std::string token = input_.substr(start, pos_ - start);
        char* end = nullptr;
        double parsed = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(parsed)) {
            return fail(error, "JSON number must be finite");
        }
        value.type = JsonValue::Number;
        value.number = parsed;
        return true;
    }

    bool consume(char expected) {
        if (pos_ < input_.size() && input_[pos_] == expected) { ++pos_; return true; }
        return false;
    }

    bool consumeLiteral(const char* literal) {
        std::size_t length = std::char_traits<char>::length(literal);
        if (input_.compare(pos_, length, literal) != 0) return false;
        pos_ += length;
        return true;
    }

    void skipWhitespace() {
        while (pos_ < input_.size()
               && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }

    bool fail(std::string& error, const std::string& message) {
        error = message + " at byte " + std::to_string(pos_);
        return false;
    }

    const std::string& input_;
    std::size_t pos_ = 0;
};

std::string escapeJson(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u00" << std::hex << std::setw(2)
                        << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string quote(const std::string& value) { return "\"" + escapeJson(value) + "\""; }

const JsonValue* field(const JsonValue& object, const std::string& name) {
    auto it = object.object.find(name);
    return it == object.object.end() ? nullptr : &it->second;
}

bool readString(const JsonValue& object, const std::string& name,
                std::string& value, std::string& error, bool required = false) {
    const JsonValue* found = field(object, name);
    if (!found) {
        if (required) error = "missing " + name;
        return !required;
    }
    if (found->type != JsonValue::String) {
        error = name + " must be a string";
        return false;
    }
    value = found->string;
    return true;
}

bool readNumber(const JsonValue& object, const std::string& name,
                double& value, std::string& error, bool& present) {
    const JsonValue* found = field(object, name);
    present = found != nullptr;
    if (!found) return true;
    if (found->type != JsonValue::Number) {
        error = name + " must be a number";
        return false;
    }
    value = found->number;
    return true;
}

bool rejectUnknown(const JsonValue& object, const std::vector<std::string>& allowed,
                   std::string& error) {
    for (const auto& entry : object.object) {
        bool known = false;
        for (const auto& name : allowed) if (entry.first == name) { known = true; break; }
        if (!known) { error = "unknown JSON field: " + entry.first; return false; }
    }
    return true;
}

bool addNumber(const JsonValue& object, const std::string& jsonKey,
               const std::string& paramKey, Policy& policy, std::string& error) {
    double value = 0.0;
    bool present = false;
    if (!readNumber(object, jsonKey, value, error, present)) return false;
    if (!present) return true;
    if (policy.params.count(paramKey)) {
        error = "duplicate parameter alias: " + paramKey;
        return false;
    }
    policy.params[paramKey] = value;
    return true;
}

std::string numberToJson(double value) {
    std::ostringstream out;
    out << std::setprecision(15) << value;
    return out.str();
}
}

DecodeRequestResult JsonCodec::decodeEvaluateRequest(const std::string& body) {
    DecodeRequestResult result;
    JsonValue root;
    if (!JsonParser(body).parse(root, result.error) || root.type != JsonValue::Object) {
        if (result.error.empty()) result.error = "request body must be a JSON object";
        return result;
    }
    if (!rejectUnknown(root, {"tenant", "tenantId", "endpoint", "role"}, result.error)) return result;
    if (field(root, "role")) { result.error = "role is not supported"; return result; }
    if (field(root, "tenant") && field(root, "tenantId")) {
        result.error = "provide tenant or tenantId, not both";
        return result;
    }
    std::string tenant;
    if (field(root, "tenant")) {
        if (!readString(root, "tenant", tenant, result.error, true)) return result;
    } else if (!readString(root, "tenantId", tenant, result.error, true)) return result;
    std::string endpoint;
    if (!readString(root, "endpoint", endpoint, result.error, true)) return result;
    if (tenant.empty()) { result.error = "missing tenant"; return result; }
    if (endpoint.empty() || endpoint.front() != '/') {
        result.error = "endpoint must be a non-empty absolute path";
        return result;
    }
    if (tenant.size() > 256 || endpoint.size() > 2048) {
        result.error = "tenant or endpoint exceeds maximum length";
        return result;
    }
    result.ok = true;
    result.request = Request(tenant, endpoint);
    return result;
}

DecodePolicyResult JsonCodec::decodePolicy(const std::string& body) {
    DecodePolicyResult result;
    JsonValue root;
    if (!JsonParser(body).parse(root, result.error) || root.type != JsonValue::Object) {
        if (result.error.empty()) result.error = "request body must be a JSON object";
        return result;
    }
    const std::vector<std::string> allowed = {
        "priority", "match", "algorithm", "params", "tenant", "tenantId",
        "endpoint", "role", "capacity", "refillRate", "refill", "limit",
        "window", "leakRate", "leak"
    };
    if (!rejectUnknown(root, allowed, result.error)) return result;
    if (field(root, "role")) { result.error = "role is not supported"; return result; }

    Policy policy;
    double priority = 0.0;
    bool priorityPresent = false;
    if (!readNumber(root, "priority", priority, result.error, priorityPresent)) return result;
    if (priorityPresent) {
        if (std::floor(priority) != priority
            || priority < std::numeric_limits<int>::min()
            || priority > std::numeric_limits<int>::max()) {
            result.error = "priority must be an integer";
            return result;
        }
        policy.priority = static_cast<int>(priority);
    }

    const JsonValue* match = field(root, "match");
    if (match && match->type != JsonValue::Object) {
        result.error = "match must be an object";
        return result;
    }
    if (match && (field(root, "tenant") || field(root, "tenantId")
                  || field(root, "endpoint"))) {
        result.error = "put match fields either in match or at the top level, not both";
        return result;
    }
    const JsonValue& matchSource = match ? *match : root;
    if (match && !rejectUnknown(*match, {"tenant", "tenantId", "endpoint", "role"}, result.error)) return result;
    if (match && field(*match, "role")) { result.error = "role is not supported"; return result; }
    if (field(matchSource, "tenant") && field(matchSource, "tenantId")) {
        result.error = "provide tenant or tenantId, not both";
        return result;
    }
    std::string tenant;
    if (field(matchSource, "tenant")) {
        if (!readString(matchSource, "tenant", tenant, result.error, true)) return result;
    } else if (field(matchSource, "tenantId")
               && !readString(matchSource, "tenantId", tenant, result.error, true)) return result;
    if (!tenant.empty()) { policy.hasTenant = true; policy.matchTenant = tenant; }
    std::string endpoint;
    if (field(matchSource, "endpoint")
        && !readString(matchSource, "endpoint", endpoint, result.error, true)) return result;
    if (!endpoint.empty()) { policy.hasEndpoint = true; policy.matchEndpoint = endpoint; }

    std::string algorithm;
    if (!readString(root, "algorithm", algorithm, result.error, true) || algorithm.empty()) return result;
    policy.algorithm = normalizeAlgorithmName(algorithm);

    const JsonValue* params = field(root, "params");
    if (params && params->type != JsonValue::Object) {
        result.error = "params must be an object";
        return result;
    }
    if (params && (field(root, "capacity") || field(root, "refillRate")
                   || field(root, "refill") || field(root, "limit")
                   || field(root, "window") || field(root, "leakRate")
                   || field(root, "leak"))) {
        result.error = "put algorithm parameters either in params or at the top level, not both";
        return result;
    }
    const JsonValue& paramSource = params ? *params : root;
    if (params && !rejectUnknown(*params,
        {"capacity", "refillRate", "refill", "limit", "window", "leakRate", "leak"},
        result.error)) return result;
    if (!addNumber(paramSource, "capacity", "capacity", policy, result.error)
        || !addNumber(paramSource, "refillRate", "refill", policy, result.error)
        || !addNumber(paramSource, "refill", "refill", policy, result.error)
        || !addNumber(paramSource, "limit", "limit", policy, result.error)
        || !addNumber(paramSource, "window", "window", policy, result.error)
        || !addNumber(paramSource, "leakRate", "leakRate", policy, result.error)
        || !addNumber(paramSource, "leak", "leakRate", policy, result.error)) return result;

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
