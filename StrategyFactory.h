#pragma once

#include "RateLimitStrategy.h"
#include "policy/Policy.h"

#include <map>
#include <memory>
#include <string>

class StateStore;

std::unique_ptr<RateLimitStrategy> createStrategy(
    const std::string& algorithm,
    const std::map<std::string, double>& params,
    std::string* error = nullptr);

std::unique_ptr<RateLimitStrategy> createStrategy(
    const std::string& algorithm,
    const std::map<std::string, double>& params,
    StateStore* store,
    const std::string& stateKey,
    std::string* error = nullptr);

std::string buildPolicyKey(const std::string& tenantId, const Policy& policy);
std::string buildPolicyKey(const std::string& tenantId,
                           const std::string& endpoint,
                           const Policy& policy);
