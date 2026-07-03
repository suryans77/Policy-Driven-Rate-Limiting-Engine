#include "RestController.h"

#include "JsonCodec.h"
#include "StrategyFactory.h"
#include "policy/PolicyLoader.h"

#include <chrono>
#include <utility>
#include <vector>

namespace {
long long toEpochMs(const std::chrono::system_clock::time_point& time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count();
}
}

RestController::RestController(PolicyResolver& resolver,
                               PolicyEngine& engine,
                               Database& db,
                               const std::string& policyFilePath)
    : resolver_(resolver),
      engine_(engine),
      db_(db),
      policyFilePath_(policyFilePath)
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
    return HttpResponse::json(404, JsonCodec::encodeError("not found"));
}

HttpResponse RestController::handleEvaluate(const HttpRequest& req) {
    auto decoded = JsonCodec::decodeEvaluateRequest(req.body);
    if (!decoded.ok) {
        return HttpResponse::json(400, JsonCodec::encodeError(decoded.error));
    }

    Policy policy;
    if (!resolver_.resolve(decoded.request, policy)) {
        return HttpResponse::json(404, JsonCodec::encodeError("no matching policy"));
    }

    std::string policyKey = buildPolicyKey(decoded.request.tenantId, policy);
    if (!engine_.hasTenant(policyKey)) {
        std::string error;
        auto strategy = createStrategy(policy.algorithm, policy.params, &error);
        if (!strategy) {
            return HttpResponse::json(400, JsonCodec::encodeError(error));
        }
        engine_.ensurePolicy(policyKey, std::move(strategy));
    }

    bool allowed = engine_.evaluate(policyKey);
    std::string algorithm = engine_.getAlgorithm(policyKey);
    std::string state = engine_.getState(policyKey);

    db_.logRequest(decoded.request.tenantId,
                   toEpochMs(decoded.request.receivedAt),
                   allowed ? "ALLOW" : "DENY",
                   algorithm,
                   state);

    return HttpResponse::json(
        200,
        JsonCodec::encodeEvaluateResponse(
            allowed, decoded.request, policyKey, algorithm, state));
}

HttpResponse RestController::handleGetPolicies(const HttpRequest&) {
    return HttpResponse::json(200, JsonCodec::encodePolicies(resolver_.policies()));
}

HttpResponse RestController::handlePostPolicy(const HttpRequest& req) {
    auto decoded = JsonCodec::decodePolicy(req.body);
    if (!decoded.ok) {
        return HttpResponse::json(400, JsonCodec::encodeError(decoded.error));
    }

    std::string error;
    auto strategy = createStrategy(decoded.policy.algorithm, decoded.policy.params, &error);
    if (!strategy) {
        return HttpResponse::json(400, JsonCodec::encodeError(error));
    }

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
