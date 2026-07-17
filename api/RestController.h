#pragma once

#include "HttpTypes.h"
#include "Database.h"
#include "PolicyEngine.h"
#include "policy/PolicyResolver.h"

#include <string>
#include <map>
#include <mutex>
#include <atomic>

class StateStore;

class RestController {
public:
    RestController(PolicyResolver& resolver,
                   PolicyEngine& engine,
                   Database& db,
                   const std::string& policyFilePath = "policy/policies.yaml",
                   StateStore* stateStore = nullptr,
                   const std::map<std::string, std::string>& tenantTokens = {},
                   const std::string& adminToken = "");

    HttpResponse handleRequest(const HttpRequest& req);
    HttpResponse handleEvaluate(const HttpRequest& req);
    HttpResponse handlePostPolicy(const HttpRequest& req);
    HttpResponse handleGetPolicies(const HttpRequest& req);
    HttpResponse handleHealth(bool readiness);
    HttpResponse handleMetrics(const HttpRequest& req);

private:
    PolicyResolver& resolver_;
    PolicyEngine& engine_;
    Database& db_;
    std::string policyFilePath_;
    StateStore* stateStore_;
    std::map<std::string, std::string> tenantTokens_;
    std::string adminToken_;
    std::mutex policyMutationMutex_;
    std::atomic<unsigned long long> evaluations_{0};
    std::atomic<unsigned long long> allowed_{0};
    std::atomic<unsigned long long> denied_{0};
    std::atomic<unsigned long long> dependencyErrors_{0};

    HttpResponse authorizeTenant(const HttpRequest& req,
                                 const std::string& tenant) const;
    HttpResponse authorizeAdmin(const HttpRequest& req) const;
};
