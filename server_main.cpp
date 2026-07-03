#include "Database.h"
#include "PolicyEngine.h"
#include "api/RestController.h"
#include "api/Server.h"
#include "policy/PolicyLoader.h"
#include "policy/PolicyResolver.h"
#include "storage/RedisStateStore.h"

#include <cstdlib>
#include <iostream>
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
}

int main() {
    std::string policyFile = envOrDefault("POLICY_FILE", "policy/policies.yaml");
    std::string dbPath = envOrDefault("SQLITE_DB_PATH", "rlimit.db");
    std::string redisHost = envOrDefault("REDIS_HOST", "localhost");
    int redisPort = envIntOrDefault("REDIS_PORT", 6379);
    int port = envIntOrDefault("HTTP_PORT", 8080);

    auto loaded = PolicyLoader::loadFromFile(policyFile);
    if (!loaded.ok) {
        std::cerr << "[POLICY ERROR] " << loaded.error << "\n";
        return 1;
    }

    PolicyResolver resolver(loaded.policies);
    PolicyEngine engine;
    Database db(dbPath);
    RedisStateStore redis(redisHost, redisPort);
    RestController controller(resolver, engine, db, policyFile, &redis);
    Server server(controller, port);
    server.run();
    return 0;
}
