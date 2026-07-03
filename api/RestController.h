#pragma once

#include "HttpTypes.h"
#include "Database.h"
#include "PolicyEngine.h"
#include "policy/PolicyResolver.h"

#include <string>

class RestController {
public:
    RestController(PolicyResolver& resolver,
                   PolicyEngine& engine,
                   Database& db,
                   const std::string& policyFilePath = "policy/policies.yaml");

    HttpResponse handleRequest(const HttpRequest& req);
    HttpResponse handleEvaluate(const HttpRequest& req);
    HttpResponse handlePostPolicy(const HttpRequest& req);
    HttpResponse handleGetPolicies(const HttpRequest& req);

private:
    PolicyResolver& resolver_;
    PolicyEngine& engine_;
    Database& db_;
    std::string policyFilePath_;
};
