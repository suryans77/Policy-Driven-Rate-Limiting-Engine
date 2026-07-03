#pragma once
#include "RateLimitStrategy.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <mutex>

class PolicyEngine {
public:
    void setPolicy(const std::string& tenantId,
                   std::unique_ptr<RateLimitStrategy> strategy);
    bool ensurePolicy(const std::string& tenantId,
                      std::unique_ptr<RateLimitStrategy> strategy);

    // Thread-safe evaluate (mutex locked)
    bool evaluate(const std::string& tenantId);

    // Intentionally unsafe — no mutex, used ONLY to demonstrate the race condition
    bool evaluateUnsafe(const std::string& tenantId);

    std::string getState(const std::string& tenantId) const;
    bool hasTenant(const std::string& tenantId) const;
    std::string getAlgorithm(const std::string& tenantId) const;

    // Reset a tenant's strategy state (for stress test re-runs)
    void resetPolicy(const std::string& tenantId);

private:
    std::unordered_map<std::string,
                       std::unique_ptr<RateLimitStrategy>> tenantPolicies_;
    mutable std::mutex engineMutex_;
};
