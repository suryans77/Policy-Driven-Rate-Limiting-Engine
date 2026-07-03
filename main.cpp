#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <map>
#include <memory>
#include <algorithm>
#include "PolicyEngine.h"
#include "TokenBucket.h"
#include "FixedWindowCounter.h"
#include "SlidingWindowLog.h"
#include "SlidingWindowCounter.h"
#include "LeakyBucket.h"
#include "Database.h"
#include "Metrics.h"

using namespace std::chrono_literals;

// ── helpers ───────────────────────────────────────────────────────────────────

void printHeader() {
    std::cout << "\n+================================+\n";
    std::cout <<   "|        Rlimit CLI v0.3         |\n";
    std::cout <<   "+================================+\n";
}

void printMenu() {
    std::cout << "\n1. Add tenant\n"
              << "2. Set rate limit policy (algorithm + params)\n"
              << "3. Simulate single request\n"
              << "4. Run burst simulation (N requests, configurable interval)\n"
              << "5. Show tenant state\n"
              << "6. Run thread-safety stress test (2 threads, 1000 req each)\n"
              << "7. Show request audit log (tenant)\n"
              << "8. Run benchmarks (throughput + concurrency variance + accuracy)\n"
              << "9. Exit\n"
              << "\nChoice: ";
}

std::chrono::steady_clock::time_point& programStart() {
    static auto start = std::chrono::steady_clock::now();
    return start;
}

long long timestampMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - programStart()).count();
}

// Relative timestamp in ms from program start
std::string timestamp() {
    std::ostringstream oss;
    oss << std::setw(7) << timestampMs() << "ms";
    return oss.str();
}

std::string makeParams(const std::string& firstKey, double firstValue,
                       const std::string& secondKey, double secondValue) {
    std::ostringstream oss;
    oss << firstKey << "=" << firstValue << "|" << secondKey << "=" << secondValue;
    return oss.str();
}

std::map<std::string, std::string> parseParams(const std::string& params) {
    std::map<std::string, std::string> parsed;
    std::stringstream ss(params);
    std::string part;
    while (std::getline(ss, part, '|')) {
        auto pos = part.find('=');
        if (pos != std::string::npos) {
            parsed[part.substr(0, pos)] = part.substr(pos + 1);
        }
    }
    return parsed;
}

std::unique_ptr<RateLimitStrategy> makeStrategy(const std::string& algorithm,
                                                const std::string& params) {
    auto p = parseParams(params);
    try {
        if (algorithm == "TokenBucket") {
            return std::make_unique<TokenBucket>(std::stod(p["capacity"]), std::stod(p["refill"]));
        }
        if (algorithm == "FixedWindowCounter") {
            return std::make_unique<FixedWindowCounter>(std::stoi(p["limit"]), std::stod(p["window"]));
        }
        if (algorithm == "SlidingWindowLog") {
            return std::make_unique<SlidingWindowLog>(std::stoi(p["limit"]), std::stod(p["window"]));
        }
        if (algorithm == "SlidingWindowCounter") {
            return std::make_unique<SlidingWindowCounter>(std::stoi(p["limit"]), std::stod(p["window"]));
        }
        if (algorithm == "LeakyBucket") {
            return std::make_unique<LeakyBucket>(std::stod(p["capacity"]), std::stod(p["leakRate"]));
        }
    } catch (...) {
        return nullptr;
    }
    return nullptr;
}

void logEvaluation(Database& db, PolicyEngine& engine, const std::string& id, bool ok) {
    db.logRequest(id, timestampMs(), ok ? "ALLOW" : "DENY",
                  engine.getAlgorithm(id), engine.getState(id));
}

void setPolicyMenu(PolicyEngine& engine, Database& db, const std::string& tenantId) {
    std::cout << "\nAlgorithm:\n"
              << "  1. TokenBucket\n"
              << "  2. FixedWindowCounter\n"
              << "  3. SlidingWindowLog\n"
              << "  4. SlidingWindowCounter\n"
              << "  5. LeakyBucket\n"
              << "Choice: ";
    int alg; std::cin >> alg;

    if (alg == 1) {
        double cap, rate;
        std::cout << "Capacity (burst ceiling): ";  std::cin >> cap;
        std::cout << "Refill rate (tokens/sec): ";  std::cin >> rate;
        engine.setPolicy(tenantId, std::make_unique<TokenBucket>(cap, rate));
        db.saveTenant(tenantId, tenantId, "TokenBucket", makeParams("capacity", cap, "refill", rate));
        std::cout << "TokenBucket set: cap=" << cap << " rate=" << rate << "/s\n";

    } else if (alg == 2) {
        int limit; double window;
        std::cout << "Limit (requests/window): ";  std::cin >> limit;
        std::cout << "Window size (seconds): ";     std::cin >> window;
        engine.setPolicy(tenantId, std::make_unique<FixedWindowCounter>(limit, window));
        db.saveTenant(tenantId, tenantId, "FixedWindowCounter", makeParams("limit", limit, "window", window));
        std::cout << "FixedWindowCounter set: limit=" << limit << " window=" << window << "s\n";

    } else if (alg == 3) {
        int limit; double window;
        std::cout << "Limit (requests/window): ";  std::cin >> limit;
        std::cout << "Window size (seconds): ";     std::cin >> window;
        engine.setPolicy(tenantId, std::make_unique<SlidingWindowLog>(limit, window));
        db.saveTenant(tenantId, tenantId, "SlidingWindowLog", makeParams("limit", limit, "window", window));
        std::cout << "SlidingWindowLog set: limit=" << limit << " window=" << window << "s\n";

    } else if (alg == 4) {
        int limit; double window;
        std::cout << "Limit (requests/window): ";  std::cin >> limit;
        std::cout << "Window size (seconds): ";     std::cin >> window;
        engine.setPolicy(tenantId, std::make_unique<SlidingWindowCounter>(limit, window));
        db.saveTenant(tenantId, tenantId, "SlidingWindowCounter", makeParams("limit", limit, "window", window));
        std::cout << "SlidingWindowCounter set: limit=" << limit << " window=" << window << "s\n";

    } else if (alg == 5) {
        double cap, rate;
        std::cout << "Capacity (queue depth): ";   std::cin >> cap;
        std::cout << "Leak rate (requests/sec): "; std::cin >> rate;
        engine.setPolicy(tenantId, std::make_unique<LeakyBucket>(cap, rate));
        db.saveTenant(tenantId, tenantId, "LeakyBucket", makeParams("capacity", cap, "leakRate", rate));
        std::cout << "LeakyBucket set: cap=" << cap << " leak=" << rate << "/s\n";

    } else {
        std::cout << "Invalid algorithm.\n";
    }
}

// ── Phase 4: burst simulation (formatted table) ───────────────────────────────

void runBurstSimulation(PolicyEngine& engine, Database& db, const std::string& id) {
    int n; double interval;
    std::cout << "Number of requests : "; std::cin >> n;
    std::cout << "Interval ms (0=instant): "; std::cin >> interval;

    std::cout << "\n";
    std::cout << "+-------+-----------+-----------------------------------------------+\n";
    std::cout << "| Req # | Time      | Result   | State                               |\n";
    std::cout << "+-------+-----------+-----------------------------------------------+\n";

    int allowed = 0, denied = 0;
    for (int i = 1; i <= n; ++i) {
        bool ok = engine.evaluate(id);
        ok ? ++allowed : ++denied;
        logEvaluation(db, engine, id, ok);

        std::cout << "| " << std::setw(5) << i
                  << " | " << timestamp()
                  << " | " << (ok ? "[ALLOWED]" : "[DENIED] ")
                  << " | " << std::left << std::setw(35) << engine.getState(id).substr(0, 35)
                  << std::right << " |\n";

        if (interval > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(interval)));
    }

    std::cout << "+-------+-----------+-----------------------------------------------+\n";
    std::cout << "  Total: " << allowed << " allowed, " << denied << " denied "
              << "(" << n << " requests)\n";
}

void showAuditLog(Database& db, const std::string& tenantId) {
    auto rows = db.getRequestLog(tenantId, 50);
    std::reverse(rows.begin(), rows.end());

    std::cout << "\n+--------+----------------+--------+----------------------+----------------------------------+\n";
    std::cout << "| Req #  | Timestamp (ms) | Result | Algorithm            | State                            |\n";
    std::cout << "+--------+----------------+--------+----------------------+----------------------------------+\n";
    int req = 1;
    for (const auto& row : rows) {
        std::cout << "| " << std::setw(6) << req++
                  << " | " << std::setw(12) << row.timestampMs << "ms"
                  << " | " << std::left << std::setw(6) << row.result
                  << " | " << std::setw(20) << row.algorithm.substr(0, 20)
                  << " | " << std::setw(32) << row.stateSnapshot.substr(0, 32)
                  << std::right << " |\n";
    }
    std::cout << "+--------+----------------+--------+----------------------+----------------------------------+\n";
    std::cout << "  Total: " << db.getTotalAllowed(tenantId) << " allowed, "
              << db.getTotalDenied(tenantId) << " denied across "
              << rows.size() << " shown / " << db.getTotalRequests(tenantId)
              << " logged requests\n";
}

void runBenchmarkReport() {
    Metrics metrics;
    auto throughput = metrics.runThroughputBenchmark();
    auto concurrency = metrics.runConcurrencyVariance();
    auto accuracy = metrics.runAccuracyComparison();

    auto fastest = std::min_element(throughput.begin(), throughput.end(),
        [](const ThroughputResult& a, const ThroughputResult& b) {
            return a.requestsPerSec < b.requestsPerSec;
        });
    auto slowest = std::max_element(throughput.begin(), throughput.end(),
        [](const ThroughputResult& a, const ThroughputResult& b) {
            return a.requestsPerSec < b.requestsPerSec;
        });

    std::cout << "\n+==========================================+\n";
    std::cout <<   "|           Rlimit Benchmark Report        |\n";
    std::cout <<   "+==========================================+\n\n";

    std::cout << "[1] Throughput (100,000 requests per algorithm)\n";
    std::cout << "+----------------------+------------+------------------+\n";
    std::cout << "| Algorithm            | Time (ms)  | Req/sec          |\n";
    std::cout << "+----------------------+------------+------------------+\n";
    for (const auto& r : throughput) {
        std::cout << "| " << std::left << std::setw(20) << r.algorithm
                  << " | " << std::right << std::setw(9) << r.totalTimeMs << "ms"
                  << " | " << std::setw(16) << std::fixed << std::setprecision(0) << r.requestsPerSec
                  << " |\n";
    }
    std::cout << "+----------------------+------------+------------------+\n";
    std::cout << "  Fastest: " << slowest->algorithm << " | Slowest: " << fastest->algorithm << "\n";
    std::cout << "  Reason: SlidingWindowLog stores and evicts timestamps; the others are O(1) state updates.\n\n";

    std::cout << "[2] Concurrency Race Variance (20 runs, 2 threads x 1000 req, NO mutex)\n";
    std::cout << "+------------------+------------------+------------------+------------------+\n";
    std::cout << "| Min Over-Allows  | Max Over-Allows  | Avg Over-Allows  | Races Fired      |\n";
    std::cout << "+------------------+------------------+------------------+------------------+\n";
    std::cout << "| " << std::setw(16) << concurrency.minOverAllows
              << " | " << std::setw(16) << concurrency.maxOverAllows
              << " | " << std::setw(16) << std::setprecision(1) << concurrency.avgOverAllows
              << " | " << std::setw(10) << concurrency.racesFired << "/20     |\n";
    std::cout << "+------------------+------------------+------------------+------------------+\n";
    std::cout << "  Interpretation: race is non-deterministic; this run fired "
              << concurrency.racesFired << "/20 times under load.\n\n";

    std::cout << "[3] SlidingWindowCounter Accuracy (5 runs x 100 requests vs SlidingWindowLog exact)\n";
    std::cout << "+--------------+----------------+-----------------+\n";
    std::cout << "| Log Allows   | Counter Allows | Avg Error %     |\n";
    std::cout << "+--------------+----------------+-----------------+\n";
    std::cout << "| " << std::setw(12) << accuracy.logAllows
              << " | " << std::setw(14) << accuracy.counterAllows
              << " | " << std::setw(14) << std::setprecision(2) << accuracy.avgErrorPercent << "% |\n";
    std::cout << "+--------------+----------------+-----------------+\n";
    std::cout << "  Avg approximation error: " << std::setprecision(2)
              << accuracy.avgErrorPercent << "% vs exact log baseline\n";
}

// ── Phase 3: thread-safety stress test ───────────────────────────────────────

void runStressTest(PolicyEngine& engine) {
    const int THREADS    = 2;
    const int REQS_EACH  = 1000;
    const int TOTAL_REQS = THREADS * REQS_EACH;  // 2000
    const int LIMIT      = 1000;                  // policy limit

    const std::string tenant = "_stress_";

    std::cout << "\n+------------------------------------------+\n";
    std::cout <<   "|       Thread-Safety Stress Test          |\n";
    std::cout <<   "+------------------------------------------+\n";
    std::cout << "  Config: " << THREADS << " threads x " << REQS_EACH
              << " requests = " << TOTAL_REQS << " total\n";
    std::cout << "  Policy: TokenBucket capacity=" << LIMIT << " refill=0\n";
    std::cout << "  Correct result: exactly " << LIMIT << " allowed\n\n";

    // ── Run 1: WITHOUT mutex (evaluateUnsafe) ─────────────────────────────────
    engine.setPolicy(tenant, std::make_unique<TokenBucket>(LIMIT, 0));
    std::atomic<int> unsafeAllowed{0};

    auto unsafeWorker = [&]() {
        for (int i = 0; i < REQS_EACH; ++i)
            if (engine.evaluateUnsafe(tenant)) ++unsafeAllowed;
    };

    {
        std::thread t1(unsafeWorker), t2(unsafeWorker);
        t1.join(); t2.join();
    }

    bool unsafeCorrect = (unsafeAllowed.load() == LIMIT);
    std::cout << "  [WITHOUT mutex]  allowed = " << unsafeAllowed.load()
              << "  (expected " << LIMIT << ")  "
              << (unsafeCorrect ? "OK (got lucky)" : "RACE DETECTED — over-allowed!") << "\n";

    // ── Run 2: WITH mutex (evaluate) ──────────────────────────────────────────
    engine.setPolicy(tenant, std::make_unique<TokenBucket>(LIMIT, 0));
    std::atomic<int> safeAllowed{0};

    auto safeWorker = [&]() {
        for (int i = 0; i < REQS_EACH; ++i)
            if (engine.evaluate(tenant)) ++safeAllowed;
    };

    {
        std::thread t1(safeWorker), t2(safeWorker);
        t1.join(); t2.join();
    }

    bool safeCorrect = (safeAllowed.load() == LIMIT);
    std::cout << "  [WITH mutex]     allowed = " << safeAllowed.load()
              << "  (expected " << LIMIT << ")  "
              << (safeCorrect ? "CORRECT" : "UNEXPECTED — mutex may be broken") << "\n";

    std::cout << "\n";
    std::cout << "  Race condition story:\n";
    std::cout << "    Thread A reads tokens=1 -> decides ALLOW\n";
    std::cout << "    Thread B reads tokens=1 -> decides ALLOW  (before A writes)\n";
    std::cout << "    Thread A writes tokens=0\n";
    std::cout << "    Thread B writes tokens=0\n";
    std::cout << "    Result: 2 requests allowed on 1 token — a real business bug.\n";
    std::cout << "    Fix: lock_guard holds mutex for the entire read-modify-write.\n";
    std::cout << "+------------------------------------------+\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    PolicyEngine engine;
    Database db;
    printHeader();

    int restored = 0;
    for (const auto& tenant : db.loadAllTenants()) {
        auto strategy = makeStrategy(tenant.algorithm, tenant.params);
        if (strategy) {
            engine.setPolicy(tenant.id, std::move(strategy));
            ++restored;
        }
    }
    if (restored > 0) {
        std::cout << "Restored " << restored << " tenant policies from SQLite.\n";
    }

    while (true) {
        printMenu();
        int choice; std::cin >> choice;

        if (choice == 1) {
            std::string id;
            std::cout << "Tenant ID: "; std::cin >> id;
            std::cout << "Tenant '" << id << "' ready — set a policy next.\n";

        } else if (choice == 2) {
            std::string id;
            std::cout << "Tenant ID: "; std::cin >> id;
            setPolicyMenu(engine, db, id);

        } else if (choice == 3) {
            std::string id;
            std::cout << "Tenant ID: "; std::cin >> id;
            if (!engine.hasTenant(id)) {
                std::cout << "Unknown tenant (set a policy first).\n";
                continue;
            }
            bool ok = engine.evaluate(id);
            logEvaluation(db, engine, id, ok);
            std::cout << "[" << timestamp() << "]  "
                      << (ok ? "[ALLOWED]" : "[DENIED] ")
                      << "  " << engine.getState(id) << "\n";

        } else if (choice == 4) {
            std::string id;
            std::cout << "Tenant ID: "; std::cin >> id;
            if (!engine.hasTenant(id)) {
                std::cout << "Unknown tenant.\n";
                continue;
            }
            runBurstSimulation(engine, db, id);

        } else if (choice == 5) {
            std::string id;
            std::cout << "Tenant ID: "; std::cin >> id;
            if (!engine.hasTenant(id)) {
                std::cout << "Unknown tenant.\n";
                continue;
            }
            std::cout << "Algorithm : " << engine.getAlgorithm(id) << "\n";
            std::cout << "State     : " << engine.getState(id) << "\n";

        } else if (choice == 6) {
            runStressTest(engine);

        } else if (choice == 7) {
            std::string id;
            std::cout << "Tenant ID: "; std::cin >> id;
            showAuditLog(db, id);

        } else if (choice == 8) {
            runBenchmarkReport();

        } else if (choice == 9) {
            std::cout << "Goodbye.\n";
            break;
        } else {
            std::cout << "Invalid choice.\n";
        }
    }

    return 0;
}

/*
 * DEMO COMMANDS — each shows one algorithm's distinctive behavior live:
 *
 * 1. TokenBucket (burst then throttle):
 *    Tenant "tb", TokenBucket cap=5 rate=1. Burst sim 10 reqs 0ms.
 *    -> 5 ALLOWED, 5 DENIED. Wait 3s, fire 3 -> all ALLOWED (refilled).
 *
 * 2. FixedWindowCounter (boundary burst exploit):
 *    Tenant "fw", FixedWindow limit=5 window=3s.
 *    Fire 5 at t=0 -> ALLOWED. Fire 5 at t=2.9s -> ALLOWED (new window).
 *    10 requests in 100ms. Both windows look clean.
 *
 * 3. SlidingWindowLog (no boundary burst):
 *    Tenant "swl", SlidingWindowLog limit=5 window=3s.
 *    Same pattern -> second burst correctly DENIED (entries still in log).
 *
 * 4. SlidingWindowCounter (weighted estimate):
 *    Tenant "swc", SlidingWindowCounter limit=10 window=10s.
 *    Fire 9 at t=0. Cross window. estimate=~9 -> only 1 slot remains.
 *
 * 5. LeakyBucket (output smoothing):
 *    Tenant "lb", LeakyBucket cap=5 leak=1. Fire 5 -> fills queue (ALLOWED).
 *    Fire 5 more instantly -> DENIED (queue full). Wait 3s -> 3 drain -> 3 ALLOWED.
 *
 * 6. Thread-safety stress test (option 6):
 *    No tenant needed. Runs automatically.
 *    Shows without-mutex over-allows, with-mutex is exact.
 */
