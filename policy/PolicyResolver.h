#pragma once

#include "Policy.h"

#include <vector>
#include <shared_mutex>

class PolicyResolver {
public:
    explicit PolicyResolver(const std::vector<Policy>& policies = {});

    void setPolicies(const std::vector<Policy>& policies);
    bool resolve(const Request& req, Policy& policy) const;
    std::vector<Policy> policies() const;

private:
    std::vector<Policy> policies_;
    mutable std::shared_mutex mutex_;
};
