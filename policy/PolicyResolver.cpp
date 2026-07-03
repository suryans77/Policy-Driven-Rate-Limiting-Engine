#include "PolicyResolver.h"

#include <algorithm>

namespace {
std::vector<Policy> sortedPolicies(std::vector<Policy> policies) {
    std::stable_sort(policies.begin(), policies.end(),
        [](const Policy& a, const Policy& b) {
            return a.priority > b.priority;
        });
    return policies;
}
}

PolicyResolver::PolicyResolver(const std::vector<Policy>& policies) {
    setPolicies(policies);
}

void PolicyResolver::setPolicies(const std::vector<Policy>& policies) {
    policies_ = sortedPolicies(policies);
}

bool PolicyResolver::resolve(const Request& req, Policy& policy) const {
    for (const auto& candidate : policies_) {
        if (candidate.matches(req)) {
            policy = candidate;
            return true;
        }
    }
    return false;
}

const std::vector<Policy>& PolicyResolver::policies() const {
    return policies_;
}
