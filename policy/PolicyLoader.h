#pragma once

#include "Policy.h"

#include <string>
#include <vector>

struct PolicyLoadResult {
    bool ok = false;
    std::vector<Policy> policies;
    std::string error;
};

class PolicyLoader {
public:
    static PolicyLoadResult loadFromFile(const std::string& path);
    static bool saveToFile(const std::string& path,
                           const std::vector<Policy>& policies,
                           std::string* error = nullptr);
    static std::string toYaml(const std::vector<Policy>& policies);
};
