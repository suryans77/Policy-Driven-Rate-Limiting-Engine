#include "RestController.h"

#include "JsonCodec.h"
#include "StrategyFactory.h"
#include "policy/PolicyLoader.h"
#include "storage/StateStore.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>
#include <vector>

namespace {
long long toEpochMs(const std::chrono::system_clock::time_point& time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count();
}

std::string headerValue(const HttpRequest& req, const std::string& wanted) {
    for (const auto& header : req.headers) {
        if (header.first.size() != wanted.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(header.first[i]))
                != std::tolower(static_cast<unsigned char>(wanted[i]))) {
                same = false;
                break;
            }
        }
        if (same) return header.second;
    }
    return "";
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    std::size_t maxLength = std::max(a.size(), b.size());
    unsigned char difference = static_cast<unsigned char>(a.size() ^ b.size());
    for (std::size_t i = 0; i < maxLength; ++i) {
        unsigned char left = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char right = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        difference |= static_cast<unsigned char>(left ^ right);
    }
    return difference == 0;
}

std::string bearerToken(const HttpRequest& req) {
    std::string value = headerValue(req, "Authorization");
    const std::string prefix = "Bearer ";
    return value.compare(0, prefix.size(), prefix) == 0 ? value.substr(prefix.size()) : "";
}

bool hasSupportedContentType(const HttpRequest& req) {
    std::string value = headerValue(req, "Content-Type");
    if (value.empty()) return true;
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "application/json"
        || value.compare(0, 17, "application/json;") == 0;
}
}

RestController::RestController(PolicyResolver& resolver,
                               PolicyEngine& engine,
                               Database& db,
                               const std::string& policyFilePath,
                               StateStore* stateStore,
                               const std::map<std::string, std::string>& tenantTokens,
                               const std::string& adminToken)
    : resolver_(resolver),
      engine_(engine),
      db_(db),
      policyFilePath_(policyFilePath),
      stateStore_(stateStore),
      tenantTokens_(tenantTokens),
      adminToken_(adminToken)
{}

HttpResponse RestController::handleRequest(const HttpRequest& req) {
    if (req.method == "POST" && req.path == "/evaluate") {
        return handleEvaluate(req);
    }
    if (req.method == "GET" && req.path == "/policies") {
        return handleGetPolicies(req);
    }
    if (req.method == "POST" && req.path == "/policies") {
        return handlePostPolicy(req);
    }
    if (req.method == "GET" && req.path == "/health/live") {
        return handleHealth(false);
    }
    if (req.method == "GET" && req.path == "/health/ready") {
        return handleHealth(true);
    }
    if (req.method == "GET" && req.path == "/metrics") {
        return handleMetrics(req);
    }
    if (req.path == "/evaluate" || req.path == "/policies"
        || req.path == "/health/live" || req.path == "/health/ready"
        || req.path == "/metrics") {
        HttpResponse response = HttpResponse::json(
            405, JsonCodec::encodeError("method not allowed"));
        response.headers["Allow"] = req.path == "/evaluate" ? "POST"
            : (req.path == "/policies" ? "GET, POST" : "GET");
        return response;
    }
    return HttpResponse::json(404, JsonCodec::encodeError("not found"));
}

HttpResponse RestController::handleEvaluate(const HttpRequest& req) {
    ++evaluations_;
    if (!hasSupportedContentType(req)) {
        return HttpResponse::json(415, JsonCodec::encodeError("content-type must be application/json"));
    }
    auto decoded = JsonCodec::decodeEvaluateRequest(req.body);
    if (!decoded.ok) {
        return HttpResponse::json(400, JsonCodec::encodeError(decoded.error));
    }

    auto auth = authorizeTenant(req, decoded.request.tenantId);
    if (auth.status != 200) return auth;

    Policy policy;
    if (!resolver_.resolve(decoded.request, policy)) {
        return HttpResponse::json(404, JsonCodec::encodeError("no matching policy"));
    }

    std::string policyKey = buildPolicyKey(
        decoded.request.tenantId, decoded.request.endpoint, policy);
    if (!engine_.hasTenant(policyKey)) {
        std::string error;
        auto strategy = createStrategy(policy.algorithm, policy.params, stateStore_, policyKey, &error);
        if (!strategy) {
            return HttpResponse::json(400, JsonCodec::encodeError(error));
        }
        engine_.ensurePolicy(policyKey, std::move(strategy));
    }

    EvaluationResult evaluation = engine_.evaluateDetailed(policyKey);
    if (!evaluation.error.empty()) {
        ++dependencyErrors_;
        return HttpResponse::json(
            503, JsonCodec::encodeError("rate limit state store unavailable"));
    }

    if (!db_.logRequest(decoded.request.tenantId,
                        toEpochMs(decoded.request.receivedAt),
                        evaluation.allowed ? "ALLOW" : "DENY",
                        evaluation.algorithm,
                        evaluation.state)) {
        ++dependencyErrors_;
        return HttpResponse::json(
            503, JsonCodec::encodeError("audit store unavailable; rate decision may have been consumed"));
    }
    if (evaluation.allowed) ++allowed_;
    else ++denied_;

    return HttpResponse::json(
        200,
        JsonCodec::encodeEvaluateResponse(
            evaluation.allowed, decoded.request, policyKey,
            evaluation.algorithm, evaluation.state));
}

HttpResponse RestController::handleGetPolicies(const HttpRequest& req) {
    auto auth = authorizeAdmin(req);
    if (auth.status != 200) return auth;
    return HttpResponse::json(200, JsonCodec::encodePolicies(resolver_.policies()));
}

HttpResponse RestController::handlePostPolicy(const HttpRequest& req) {
    auto auth = authorizeAdmin(req);
    if (auth.status != 200) return auth;
    if (!hasSupportedContentType(req)) {
        return HttpResponse::json(415, JsonCodec::encodeError("content-type must be application/json"));
    }
    auto decoded = JsonCodec::decodePolicy(req.body);
    if (!decoded.ok) {
        return HttpResponse::json(400, JsonCodec::encodeError(decoded.error));
    }

    std::string error;
    auto strategy = createStrategy(decoded.policy.algorithm, decoded.policy.params, &error);
    if (!strategy) {
        return HttpResponse::json(400, JsonCodec::encodeError(error));
    }

    std::lock_guard<std::mutex> mutationLock(policyMutationMutex_);
    std::vector<Policy> updated = resolver_.policies();
    updated.push_back(decoded.policy);
    PolicyResolver sorted(updated);

    if (!policyFilePath_.empty()
        && !PolicyLoader::saveToFile(policyFilePath_, sorted.policies(), &error)) {
        return HttpResponse::json(500, JsonCodec::encodeError(error));
    }

    resolver_.setPolicies(sorted.policies());
    return HttpResponse::json(201, JsonCodec::encodePolicy(decoded.policy));
}

HttpResponse RestController::handleHealth(bool readiness) {
    if (!readiness) {
        return HttpResponse::json(200, "{\"status\":\"ok\"}");
    }
    std::string error;
    bool stateHealthy = !stateStore_ || stateStore_->health(error);
    bool auditHealthy = db_.isAvailable();
    if (stateHealthy && auditHealthy) {
        return HttpResponse::json(200, "{\"status\":\"ready\"}");
    }
    return HttpResponse::json(503, "{\"status\":\"unavailable\"}");
}

HttpResponse RestController::handleMetrics(const HttpRequest& req) {
    auto auth = authorizeAdmin(req);
    if (auth.status != 200) return auth;
    std::ostringstream out;
    out << "{"
        << "\"evaluations\":" << evaluations_.load() << ","
        << "\"allowed\":" << allowed_.load() << ","
        << "\"denied\":" << denied_.load() << ","
        << "\"dependencyErrors\":" << dependencyErrors_.load()
        << "}";
    return HttpResponse::json(200, out.str());
}

HttpResponse RestController::authorizeTenant(const HttpRequest& req,
                                             const std::string& tenant) const {
    if (tenantTokens_.empty()) return HttpResponse::json(200, "{}");
    std::string token = bearerToken(req);
    if (token.empty()) {
        return HttpResponse::json(401, JsonCodec::encodeError("authentication required"));
    }
    auto expected = tenantTokens_.find(tenant);
    if (expected == tenantTokens_.end() || !constantTimeEqual(token, expected->second)) {
        return HttpResponse::json(403, JsonCodec::encodeError("tenant access denied"));
    }
    return HttpResponse::json(200, "{}");
}

HttpResponse RestController::authorizeAdmin(const HttpRequest& req) const {
    if (adminToken_.empty()) return HttpResponse::json(200, "{}");
    std::string token = bearerToken(req);
    if (token.empty()) {
        return HttpResponse::json(401, JsonCodec::encodeError("authentication required"));
    }
    if (!constantTimeEqual(token, adminToken_)) {
        return HttpResponse::json(403, JsonCodec::encodeError("admin access denied"));
    }
    return HttpResponse::json(200, "{}");
}
