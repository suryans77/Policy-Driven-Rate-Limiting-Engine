#include "PolicyEngine.h"

void PolicyEngine::setPolicy(const std::string& tenantId,
                              std::unique_ptr<RateLimitStrategy> strategy) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    tenantPolicies_[tenantId] = std::move(strategy);
}

bool PolicyEngine::evaluate(const std::string& tenantId) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    auto it = tenantPolicies_.find(tenantId);
    if (it == tenantPolicies_.end()) return false;
    return it->second->allowRequest();
}

bool PolicyEngine::evaluateUnsafe(const std::string& tenantId) {
    // NO MUTEX — race condition is intentional here for demonstration
    auto it = tenantPolicies_.find(tenantId);
    if (it == tenantPolicies_.end()) return false;
    return it->second->allowRequest();
}

std::string PolicyEngine::getState(const std::string& tenantId) const {
    std::lock_guard<std::mutex> lock(engineMutex_);
    auto it = tenantPolicies_.find(tenantId);
    if (it == tenantPolicies_.end()) return "No policy set";
    return it->second->getState();
}

bool PolicyEngine::hasTenant(const std::string& tenantId) const {
    std::lock_guard<std::mutex> lock(engineMutex_);
    return tenantPolicies_.count(tenantId) > 0;
}

std::string PolicyEngine::getAlgorithm(const std::string& tenantId) const {
    std::lock_guard<std::mutex> lock(engineMutex_);
    auto it = tenantPolicies_.find(tenantId);
    if (it == tenantPolicies_.end()) return "None";
    return it->second->algorithmName();
}

void PolicyEngine::resetPolicy(const std::string& tenantId) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    auto it = tenantPolicies_.find(tenantId);
    if (it != tenantPolicies_.end())
        it->second->reset();
}
