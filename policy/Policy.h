#pragma once

#include "Request.h"

#include <cstddef>
#include <map>
#include <string>

struct Policy {
    int priority = 0;

    bool hasTenant = false;
    std::string matchTenant;

    bool hasEndpoint = false;
    std::string matchEndpoint;

    std::string algorithm;
    std::map<std::string, double> params;

    std::size_t order = 0;

    bool matches(const Request& req) const;
    std::string paramsString() const;
};

std::string normalizeAlgorithmName(const std::string& algorithm);
