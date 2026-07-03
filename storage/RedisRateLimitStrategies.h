#pragma once

#include "RateLimitStrategy.h"
#include "StateStore.h"

#include <map>
#include <memory>
#include <string>

std::unique_ptr<RateLimitStrategy> createRedisBackedStrategy(
    const std::string& algorithm,
    const std::map<std::string, double>& params,
    StateStore& store,
    const std::string& stateKey,
    std::string* error = nullptr);
