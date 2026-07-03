#pragma once

#include "RateLimitStrategy.h"
#include "policy/Policy.h"

#include <map>
#include <memory>
#include <string>

std::unique_ptr<RateLimitStrategy> createStrategy(
    const std::string& algorithm,
    const std::map<std::string, double>& params,
    std::string* error = nullptr);

std::string buildPolicyKey(const std::string& tenantId, const Policy& policy);
