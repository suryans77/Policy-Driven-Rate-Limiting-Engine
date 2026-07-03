#pragma once

#include "Policy.h"

#include <vector>

class PolicyResolver {
public:
    explicit PolicyResolver(const std::vector<Policy>& policies = {});

    void setPolicies(const std::vector<Policy>& policies);
    bool resolve(const Request& req, Policy& policy) const;
    const std::vector<Policy>& policies() const;

private:
    std::vector<Policy> policies_;
};
