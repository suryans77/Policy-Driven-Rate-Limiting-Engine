#include "Database.h"
#include "PolicyEngine.h"
#include "api/RestController.h"
#include "api/Server.h"
#include "policy/PolicyLoader.h"
#include "policy/PolicyResolver.h"
#include "storage/RedisStateStore.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {
std::string envOrDefault(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

int envIntOrDefault(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value ? std::atoi(value) : fallback;
}

std::map<std::string, std::string> parseTenantTokens(const std::string& value,
                                                      std::string& error) {
    std::map<std::string, std::string> tokens;
    std::stringstream input(value);
    std::string entry;
    while (std::getline(input, entry, ';')) {
        if (entry.empty()) continue;
        auto separator = entry.find('=');
        if (separator == std::string::npos || separator == 0
            || separator + 1 >= entry.size()) {
            error = "TENANT_API_KEYS must use tenant=token;tenant=token";
            return {};
        }
        std::string tenant = entry.substr(0, separator);
        std::string token = entry.substr(separator + 1);
        if (token.size() < 16) {
            error = "tenant API tokens must contain at least 16 characters";
            return {};
        }
        if (!tokens.emplace(tenant, token).second) {
            error = "duplicate tenant in TENANT_API_KEYS: " + tenant;
            return {};
        }
    }
    if (tokens.empty()) error = "TENANT_API_KEYS must configure at least one tenant";
    return tokens;
}
}

int main() {
    std::string policyFile = envOrDefault("POLICY_FILE", "policy/policies.yaml");
    std::string dbPath = envOrDefault("AUDIT_STORE_PATH", "rlimit_store.txt");
    std::string redisHost = envOrDefault("REDIS_HOST", "localhost");
    std::string redisPassword = envOrDefault("REDIS_PASSWORD", "");
    std::string adminToken = envOrDefault("ADMIN_API_KEY", "");
    std::string tenantTokenConfig = envOrDefault("TENANT_API_KEYS", "");
    int redisPort = envIntOrDefault("REDIS_PORT", 6379);
    int port = envIntOrDefault("HTTP_PORT", 8080);
    int workers = envIntOrDefault("HTTP_WORKERS", 4);
    int maxQueue = envIntOrDefault("HTTP_MAX_QUEUE", 256);

    if (port <= 0 || port > 65535 || redisPort <= 0 || redisPort > 65535
        || workers <= 0 || workers > 256 || maxQueue <= 0) {
        std::cerr << "[CONFIG ERROR] HTTP_PORT and REDIS_PORT must be valid TCP ports\n";
        return 1;
    }
    if (redisPassword.size() < 16 || adminToken.size() < 16) {
        std::cerr << "[CONFIG ERROR] REDIS_PASSWORD and ADMIN_API_KEY must each contain at least 16 characters\n";
        return 1;
    }
    std::string tokenError;
    auto tenantTokens = parseTenantTokens(tenantTokenConfig, tokenError);
    if (!tokenError.empty()) {
        std::cerr << "[CONFIG ERROR] " << tokenError << "\n";
        return 1;
    }

    auto loaded = PolicyLoader::loadFromFile(policyFile);
    if (!loaded.ok) {
        std::cerr << "[POLICY ERROR] " << loaded.error << "\n";
        return 1;
    }

    PolicyResolver resolver(loaded.policies);
    PolicyEngine engine;
    Database db(dbPath);
    if (!db.isAvailable()) {
        std::cerr << "[AUDIT ERROR] audit store is required by the REST service\n";
        return 1;
    }
    RedisStateStore redis(redisHost, redisPort, redisPassword);
    std::string redisError;
    if (!redis.health(redisError)) {
        std::cerr << "[REDIS ERROR] " << redisError << "\n";
        return 1;
    }
    RestController controller(
        resolver, engine, db, policyFile, &redis, tenantTokens, adminToken);
    Server server(controller, port, workers, maxQueue);
    return server.run() ? 0 : 1;
}
