#include "PolicyResolver.h"

#include <algorithm>
#include <mutex>

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
    auto sorted = sortedPolicies(policies);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    policies_ = std::move(sorted);
}

bool PolicyResolver::resolve(const Request& req, Policy& policy) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& candidate : policies_) {
        if (candidate.matches(req)) {
            policy = candidate;
            return true;
        }
    }
    return false;
}

std::vector<Policy> PolicyResolver::policies() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return policies_;
}
