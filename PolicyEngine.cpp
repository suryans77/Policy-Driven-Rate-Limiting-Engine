#include "PolicyEngine.h"

void PolicyEngine::setPolicy(const std::string& tenantId,
                              std::unique_ptr<RateLimitStrategy> strategy) {
    std::unique_lock<std::shared_mutex> lock(registryMutex_);
    tenantPolicies_[tenantId] = std::make_shared<Entry>(std::move(strategy));
}

bool PolicyEngine::ensurePolicy(const std::string& tenantId,
                                std::unique_ptr<RateLimitStrategy> strategy) {
    std::unique_lock<std::shared_mutex> lock(registryMutex_);
    if (tenantPolicies_.count(tenantId) > 0) return false;
    tenantPolicies_[tenantId] = std::make_shared<Entry>(std::move(strategy));
    return true;
}

bool PolicyEngine::evaluate(const std::string& tenantId) {
    return evaluateDetailed(tenantId).allowed;
}

EvaluationResult PolicyEngine::evaluateDetailed(const std::string& tenantId) {
    EvaluationResult result;
    auto entry = findEntry(tenantId);
    if (!entry) return result;

    std::lock_guard<std::mutex> lock(entry->mutex);
    result.found = true;
    result.allowed = entry->strategy->allowRequest();
    result.algorithm = entry->strategy->algorithmName();
    result.state = entry->strategy->getState();
    result.error = entry->strategy->lastError();
    return result;
}

bool PolicyEngine::evaluateUnsafe(const std::string& tenantId) {
    // NO MUTEX — race condition is intentional here for demonstration
    auto entry = findEntry(tenantId);
    if (!entry) return false;
    return entry->strategy->allowRequest();
}

std::string PolicyEngine::getState(const std::string& tenantId) const {
    auto entry = findEntry(tenantId);
    if (!entry) return "No policy set";
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->strategy->getState();
}

bool PolicyEngine::hasTenant(const std::string& tenantId) const {
    std::shared_lock<std::shared_mutex> lock(registryMutex_);
    return tenantPolicies_.count(tenantId) > 0;
}

std::string PolicyEngine::getAlgorithm(const std::string& tenantId) const {
    auto entry = findEntry(tenantId);
    if (!entry) return "None";
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->strategy->algorithmName();
}

void PolicyEngine::resetPolicy(const std::string& tenantId) {
    auto entry = findEntry(tenantId);
    if (!entry) return;
    std::lock_guard<std::mutex> lock(entry->mutex);
    entry->strategy->reset();
}

std::shared_ptr<PolicyEngine::Entry> PolicyEngine::findEntry(
    const std::string& tenantId) const {
    std::shared_lock<std::shared_mutex> lock(registryMutex_);
    auto it = tenantPolicies_.find(tenantId);
    return it == tenantPolicies_.end() ? nullptr : it->second;
}
