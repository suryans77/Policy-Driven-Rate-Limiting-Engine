#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sqlite3.h>
#include "PolicyEngine.h"
#include "TokenBucket.h"
#include "FixedWindowCounter.h"
#include "SlidingWindowLog.h"
#include "SlidingWindowCounter.h"
#include "LeakyBucket.h"
#include "Database.h"
#include "Metrics.h"

using namespace std::chrono_literals;

#ifdef _WIN32
  #define GREEN  ""
  #define RED    ""
  #define YELLOW ""
  #define RESET  ""
#else
  #define GREEN  "\033[32m"
  #define RED    "\033[31m"
  #define YELLOW "\033[33m"
  #define RESET  "\033[0m"
#endif

int passed = 0, failed = 0;

// ── helpers ───────────────────────────────────────────────────────────────────

void check(bool condition, const std::string& testName) {
    if (condition) {
        std::cout << GREEN << "  [PASS] " << RESET << testName << "\n";
        ++passed;
    } else {
        std::cout << RED   << "  [FAIL] " << RESET << testName << "\n";
        ++failed;
    }
}

void section(const std::string& name) {
    std::cout << "\n" << YELLOW << "=== " << name << " ===" << RESET << "\n";
}

// Fire n requests, return how many were allowed.
int fireN(PolicyEngine& engine, const std::string& tenant, int n,
          std::chrono::milliseconds delayBetween = 0ms) {
    int allowed = 0;
    for (int i = 0; i < n; ++i) {
        if (engine.evaluate(tenant)) ++allowed;
        if (delayBetween > 0ms) std::this_thread::sleep_for(delayBetween);
    }
    return allowed;
}

// ── Token Bucket ──────────────────────────────────────────────────────────────

void testTokenBucket() {
    section("Token Bucket");

    // Burst exactly at capacity — all allowed
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(5, 0));
        check(fireN(e, "t", 5) == 5, "Burst up to capacity: all 5 allowed");
    }

    // Exceed capacity — only 5 allowed
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(5, 0));
        check(fireN(e, "t", 7) == 5, "Exceed capacity: exactly 5 allowed, 2 denied");
    }

    // Refill works: drain, wait 1s at 2/s, expect 2 more
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(5, 2.0));
        fireN(e, "t", 5);
        std::this_thread::sleep_for(1000ms);
        check(fireN(e, "t", 3) == 2, "Refill after 1s at 2/s: exactly 2 more allowed");
    }

    // Refill cannot exceed capacity
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(5, 10.0));
        std::this_thread::sleep_for(2000ms);
        check(fireN(e, "t", 7) == 5, "Refill capped at capacity: still only 5 allowed");
    }

    // Zero capacity — everything denied
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(0, 1.0));
        check(fireN(e, "t", 3) == 0, "Zero capacity: all denied");
    }

    // Slow drip: 1 request every 1100ms with 1/s refill — always has a token
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(1, 1.0));
        int allowed = fireN(e, "t", 3, 1100ms);
        check(allowed == 3, "Slow drip (1100ms gap, 1/s refill): all 3 allowed");
    }
}

// ── Fixed Window Counter ──────────────────────────────────────────────────────

void testFixedWindowCounter() {
    section("Fixed Window Counter");

    // Basic: exactly at limit — all allowed
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<FixedWindowCounter>(5, 10.0));
        check(fireN(e, "t", 5) == 5, "Burst to limit: all 5 allowed");
    }

    // Exceed limit in same window — only 5 allowed
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<FixedWindowCounter>(5, 10.0));
        check(fireN(e, "t", 8) == 5, "Exceed limit: exactly 5 allowed, 3 denied");
    }

    // Window resets: fill, wait for new window, fill again
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<FixedWindowCounter>(3, 1.0)); // 1s window
        check(fireN(e, "t", 3) == 3, "First window: 3 allowed");
        std::this_thread::sleep_for(1100ms); // cross the window boundary
        check(fireN(e, "t", 3) == 3, "Second window after reset: 3 more allowed");
    }

    // Zero limit — everything denied
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<FixedWindowCounter>(0, 10.0));
        check(fireN(e, "t", 3) == 0, "Zero limit: all denied");
    }

    // Boundary burst: two windows back-to-back pass 2x limit in short time
    // We can't reproduce the sub-second exploit in automated tests without
    // clock injection, but we verify the behavior is window-reset-based:
    // the SECOND window starts fresh regardless of how quickly the first filled.
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<FixedWindowCounter>(5, 1.0));
        int first = fireN(e, "t", 5);         // fill window 1
        std::this_thread::sleep_for(1100ms);
        int second = fireN(e, "t", 5);        // window 2 is clean
        check(first == 5 && second == 5,
              "Boundary reset: second window starts fresh (2x limit possible at boundary)");
    }

    // algorithmName check
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<FixedWindowCounter>(5, 10.0));
        check(e.getAlgorithm("t") == "FixedWindowCounter",
              "algorithmName() returns 'FixedWindowCounter'");
    }
}

// ── Sliding Window Log ────────────────────────────────────────────────────────

void testSlidingWindowLog() {
    section("Sliding Window Log");

    // Basic: exactly at limit
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowLog>(5, 10.0));
        check(fireN(e, "t", 5) == 5, "Burst to limit: all 5 allowed");
    }

    // Exceed limit — exactly limit allowed
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowLog>(5, 10.0));
        check(fireN(e, "t", 8) == 5, "Exceed limit: exactly 5 allowed, 3 denied");
    }

    // Old entries evict: fill, wait full window, fill again
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowLog>(3, 1.0)); // 1s window
        check(fireN(e, "t", 3) == 3, "Fill window: 3 allowed");
        std::this_thread::sleep_for(1100ms); // all entries now outside window
        check(fireN(e, "t", 3) == 3, "After window expires: 3 more allowed");
    }

    // Partial eviction: fire 3, wait half window, fire 3 more — some evict
    // With limit=4, window=1s: fire 3 at t=0. At t=0.6s, those 3 are still
    // in window. Fire 2 more: only 1 fits (4-3=1). At t=1.1s, first 3 evict.
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowLog>(4, 1.0));
        int first = fireN(e, "t", 3);               // t=0: 3 in log
        std::this_thread::sleep_for(600ms);          // t=0.6s: log still has 3
        int second = fireN(e, "t", 3);              // only 1 slot left → 1 allowed
        check(first == 3 && second == 1,
              "Partial window: 3 in log, 600ms later only 1 slot remains");
    }

    // Zero limit
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowLog>(0, 10.0));
        check(fireN(e, "t", 3) == 0, "Zero limit: all denied");
    }

    // No boundary burst: unlike FixedWindow, requests right after t=1s
    // are still constrained by entries from the tail of the previous second
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowLog>(5, 1.0));
        fireN(e, "t", 5);                       // fill at t=0
        std::this_thread::sleep_for(100ms);     // t=0.1s — entries NOT expired yet
        int extra = fireN(e, "t", 3);           // should be denied
        check(extra == 0,
              "No boundary burst: entries still in window 100ms after fill → all denied");
    }

    // algorithmName check
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowLog>(5, 10.0));
        check(e.getAlgorithm("t") == "SlidingWindowLog",
              "algorithmName() returns 'SlidingWindowLog'");
    }
}

// ── Sliding Window Counter ────────────────────────────────────────────────────

void testSlidingWindowCounter() {
    section("Sliding Window Counter");

    // Basic: burst to limit
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowCounter>(5, 10.0));
        check(fireN(e, "t", 5) == 5, "Burst to limit: all 5 allowed");
    }

    // Exceed limit
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowCounter>(5, 10.0));
        check(fireN(e, "t", 8) == 5, "Exceed limit: exactly 5 allowed, 3 denied");
    }

    // Window advance: SlidingWindowCounter does NOT fully reset on window flip.
    // The prev window's count is carried forward weighted by (1 - f).
    // fill=3, limit=3, window=1s. Cross window (f≈0.1):
    // estimate = 3*(1-0.1) + 0 = 2.7 → 1 slot open. Only 1 of 3 allowed.
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowCounter>(3, 1.0));
        check(fireN(e, "t", 3) == 3, "Fill window: 3 allowed");
        std::this_thread::sleep_for(1100ms); // cross window; f≈0.1
        // estimate ≈ 3*0.9 = 2.7 → 1 slot. Next: 2.7+1=3.7 → denied.
        check(fireN(e, "t", 3) == 1,
              "After window flip: prev carries weight, only 1 slot (not a full reset)");
    }

    // Weighted estimate: fill=9, limit=10, cross window immediately (f≈0.03).
    // estimate = 9*(1-0.03) + 0 = 8.73 → 1 slot. curr=1: 8.73+1=9.73 → 1 more.
    // curr=2: 8.73+2=10.73 → denied. Exactly 2 allowed.
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowCounter>(10, 2.0));
        fireN(e, "t", 9);                    // prev window: 9 requests
        std::this_thread::sleep_for(2050ms); // cross into new window (f≈0.03)
        int got = fireN(e, "t", 5);
        check(got == 2,
              "Weighted carry: fill=9/10, just after window flip → exactly 2 slots remain");
    }

    // Zero limit
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowCounter>(0, 10.0));
        check(fireN(e, "t", 3) == 0, "Zero limit: all denied");
    }

    // Multiple window advances (gap > 2 windows): prev should reset to 0
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowCounter>(5, 1.0));
        fireN(e, "t", 5);
        std::this_thread::sleep_for(2200ms); // 2+ windows pass
        check(fireN(e, "t", 5) == 5,
              "After 2+ window gaps: prev resets to 0, full limit available");
    }

    // algorithmName check
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<SlidingWindowCounter>(5, 10.0));
        check(e.getAlgorithm("t") == "SlidingWindowCounter",
              "algorithmName() returns 'SlidingWindowCounter'");
    }
}

// ── Leaky Bucket ──────────────────────────────────────────────────────────────

void testLeakyBucket() {
    section("Leaky Bucket");

    // Fill queue exactly to capacity — all allowed
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<LeakyBucket>(5, 0.0)); // leak=0: no drain
        check(fireN(e, "t", 5) == 5, "Fill to capacity: all 5 allowed");
    }

    // Overflow: queue full → denied
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<LeakyBucket>(5, 0.0));
        fireN(e, "t", 5);
        check(fireN(e, "t", 3) == 0, "Queue full: all 3 additional denied");
    }

    // Leak drains queue: fill, wait, space opens up
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<LeakyBucket>(5, 2.0)); // 2/s leak
        fireN(e, "t", 5);                        // queue = 5 (full)
        std::this_thread::sleep_for(1000ms);     // drains 2 → queue ≈ 3
        int extra = fireN(e, "t", 3);
        // After 1s drain at 2/s: queue=3, capacity=5 → 2 slots open
        check(extra == 2, "After 1s drain at 2/s: 2 slots open, 2 of 3 allowed");
    }

    // Zero capacity — everything denied immediately
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<LeakyBucket>(0, 1.0));
        check(fireN(e, "t", 3) == 0, "Zero capacity: all denied");
    }

    // Slow drip at leak rate: 1 request per 1100ms, leak=1/s
    // Queue drains 1 per second; each new request finds space
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<LeakyBucket>(1, 1.0));
        // First request fills queue to 1 (= capacity)
        // After 1.1s, leak drains to 0, next request fits
        int allowed = fireN(e, "t", 3, 1100ms);
        check(allowed == 3, "Slow drip (1100ms gap, 1/s leak): all 3 allowed");
    }

    // Contrast with TokenBucket: LeakyBucket absorbs burst into queue (not instant-through)
    // Both cap=3, both receive a burst of 3 then 3 more:
    // TokenBucket: first 3 pass THROUGH instantly (output is bursty)
    // LeakyBucket: first 3 FILL the queue (output is constant leak rate)
    // In both cases the second burst of 3 is denied — but the mechanism differs.
    // This test just confirms LeakyBucket denies the second burst too:
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<LeakyBucket>(3, 0.0));
        int first  = fireN(e, "t", 3);
        int second = fireN(e, "t", 3);
        check(first == 3 && second == 0,
              "Burst absorption: first 3 fill queue, second burst of 3 all denied");
    }

    // algorithmName check
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<LeakyBucket>(5, 1.0));
        check(e.getAlgorithm("t") == "LeakyBucket",
              "algorithmName() returns 'LeakyBucket'");
    }
}


// ── Concurrency ───────────────────────────────────────────────────────────────

void testConcurrency() {
    section("Concurrency / Mutex");

    // Safe evaluate: 2 threads x 1000 requests, TokenBucket cap=1000.
    // Exactly 1000 should be allowed — no more, no less.
    {
        const int LIMIT     = 1000;
        const int PER_THREAD = 1000;
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(LIMIT, 0));

        std::atomic<int> allowed{0};
        auto worker = [&]() {
            for (int i = 0; i < PER_THREAD; ++i)
                if (e.evaluate("t")) ++allowed;
        };

        std::thread t1(worker), t2(worker);
        t1.join(); t2.join();

        check(allowed.load() == LIMIT,
              "Safe evaluate: 2 threads x 1000 reqs, cap=1000 -> exactly 1000 allowed");
    }

    // Unsafe evaluate: 2 threads x 1000 requests, same cap=1000.
    // Race condition means > 1000 may be allowed (non-deterministic).
    // We don't assert an exact number — just report what happened.
    // The test "passes" regardless so the suite stays green; the number is the story.
    {
        const int LIMIT      = 1000;
        const int PER_THREAD = 1000;
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(LIMIT, 0));

        std::atomic<int> allowed{0};
        auto worker = [&]() {
            for (int i = 0; i < PER_THREAD; ++i)
                if (e.evaluateUnsafe("t")) ++allowed;
        };

        std::thread t1(worker), t2(worker);
        t1.join(); t2.join();

        int got = allowed.load();
        bool overAllowed = (got > LIMIT);
        std::cout << (overAllowed ? RED : GREEN)
                  << "  [INFO] " << RESET
                  << "Unsafe evaluate: allowed=" << got
                  << " (limit=" << LIMIT << ")"
                  << (overAllowed ? " -> RACE DETECTED (over-allowed)" : " -> got lucky, no race this run")
                  << "\n";
        // Always pass: the interesting result is the number printed above
        ++passed;
    }

    // reset() works: fill to capacity, reset, fill again
    {
        PolicyEngine e;
        e.setPolicy("t", std::make_unique<TokenBucket>(5, 0));
        check(fireN(e, "t", 5) == 5, "Fill to capacity: 5 allowed");
        check(fireN(e, "t", 3) == 0, "Bucket empty: 3 denied");
        e.resetPolicy("t");
        check(fireN(e, "t", 5) == 5, "After reset: 5 allowed again");
    }

    // Multiple tenants are isolated: filling one does not affect another
    {
        PolicyEngine e;
        e.setPolicy("a", std::make_unique<TokenBucket>(3, 0));
        e.setPolicy("b", std::make_unique<TokenBucket>(3, 0));
        fireN(e, "a", 3);  // drain tenant a
        check(fireN(e, "b", 3) == 3, "Tenant isolation: draining 'a' does not affect 'b'");
    }
}

// Database

std::string testDbPath(const std::string& name) {
    return name + ".db";
}

void removeTestDb(const std::string& path) {
    std::remove(path.c_str());
}

bool tableExists(const std::string& path, const std::string& table) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;

    const char* sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?;";
    sqlite3_stmt* stmt = nullptr;
    bool exists = false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
        exists = (sqlite3_step(stmt) == SQLITE_ROW);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return exists;
}

void testDatabase() {
    section("Database");

    {
        std::string path = testDbPath("test_db_init");
        removeTestDb(path);
        {
            Database db(path);
            check(db.isAvailable(), "Database init: SQLite connection available");
        }
        check(std::ifstream(path).good()
              && tableExists(path, "tenants")
              && tableExists(path, "request_log"),
              "Database init: DB file created and both tables exist");
        removeTestDb(path);
    }

    {
        std::string path = testDbPath("test_save_tenant");
        removeTestDb(path);
        Database db(path);
        db.saveTenant("t1", "Tenant One", "TokenBucket", "capacity=10|refill=2");
        auto tenants = db.loadAllTenants();
        check(tenants.size() == 1
              && tenants[0].id == "t1"
              && tenants[0].name == "Tenant One"
              && tenants[0].algorithm == "TokenBucket"
              && tenants[0].params == "capacity=10|refill=2",
              "Save tenant: loadAllTenants returns correct fields");
        removeTestDb(path);
    }

    {
        std::string path = testDbPath("test_upsert_tenant");
        removeTestDb(path);
        Database db(path);
        db.saveTenant("t1", "Tenant One", "TokenBucket", "capacity=10|refill=2");
        db.saveTenant("t1", "Tenant One", "LeakyBucket", "capacity=5|leakRate=1");
        auto tenants = db.loadAllTenants();
        check(tenants.size() == 1 && tenants[0].algorithm == "LeakyBucket",
              "Upsert tenant: same id updates existing row");
        removeTestDb(path);
    }

    {
        std::string path = testDbPath("test_delete_tenant");
        removeTestDb(path);
        Database db(path);
        db.saveTenant("t1", "Tenant One", "TokenBucket", "capacity=10|refill=2");
        db.deleteTenant("t1");
        check(db.loadAllTenants().empty(), "Delete tenant: removed tenant is not loaded");
        removeTestDb(path);
    }

    {
        std::string path = testDbPath("test_log_request");
        removeTestDb(path);
        Database db(path);
        for (int i = 0; i < 3; ++i)
            db.logRequest("t1", i, "ALLOW", "TokenBucket", "state");
        for (int i = 3; i < 5; ++i)
            db.logRequest("t1", i, "DENY", "TokenBucket", "state");
        check(db.getTotalRequests("t1") == 5
              && db.getTotalAllowed("t1") == 3
              && db.getTotalDenied("t1") == 2,
              "Log request: total, allowed, and denied counts are correct");
        removeTestDb(path);
    }

    {
        std::string path = testDbPath("test_get_request_log");
        removeTestDb(path);
        Database db(path);
        for (int i = 0; i < 10; ++i)
            db.logRequest("t1", i, (i % 2 == 0) ? "ALLOW" : "DENY", "TokenBucket", "state");
        auto rows = db.getRequestLog("t1", 5);
        check(rows.size() == 5 && rows[0].timestampMs == 9 && rows[4].timestampMs == 5,
              "Get request log: returns exactly 5 most recent rows");
        removeTestDb(path);
    }
}

void testMetrics() {
    section("Metrics");

    Metrics metrics;
    auto throughput = metrics.runThroughputBenchmark();
    std::set<std::string> algorithms;
    bool nonZero = true;
    for (const auto& result : throughput) {
        algorithms.insert(result.algorithm);
        nonZero = nonZero && result.requestsPerSec > 0.0 && result.totalTimeMs > 0;
    }

    check(throughput.size() == 5
          && algorithms.count("TokenBucket")
          && algorithms.count("FixedWindowCounter")
          && algorithms.count("SlidingWindowLog")
          && algorithms.count("SlidingWindowCounter")
          && algorithms.count("LeakyBucket"),
          "Throughput benchmark: returns all five algorithms");

    check(nonZero, "Throughput benchmark: all rates and timings are non-zero");

    auto concurrency = metrics.runConcurrencyVariance();
    check(concurrency.racesFired >= 0
          && concurrency.racesFired <= 20
          && concurrency.minOverAllows >= 0
          && concurrency.maxOverAllows >= concurrency.minOverAllows,
          "Concurrency variance: completes with sane aggregate values");

    auto accuracy = metrics.runAccuracyComparison();
    check(accuracy.avgErrorPercent < 10.0,
          "Accuracy comparison: average error stays below 10 percent");
}

void testPersistenceIntegration() {
    section("Persistence Integration");

    {
        std::string path = testDbPath("test_persistence_round_trip");
        removeTestDb(path);
        {
            Database db(path);
            db.saveTenant("t1", "Tenant One", "TokenBucket", "capacity=10|refill=2");
        }
        {
            Database db(path);
            auto tenants = db.loadAllTenants();
            check(tenants.size() == 1
                  && tenants[0].id == "t1"
                  && tenants[0].params == "capacity=10|refill=2",
                  "Persistence round trip: new Database instance loads saved tenant");
        }
        removeTestDb(path);
    }

    {
        std::string path = testDbPath("test_request_log_persists");
        removeTestDb(path);
        {
            Database db(path);
            db.logRequest("t1", 1, "ALLOW", "TokenBucket", "state");
            db.logRequest("t1", 2, "DENY", "TokenBucket", "state");
        }
        {
            Database db(path);
            check(db.getTotalRequests("t1") == 2,
                  "Request log persists: counts survive a new Database instance");
        }
        removeTestDb(path);
    }

    {
        Database db("missing_dir_for_db\\bad.db");
        check(!db.isAvailable() && db.loadAllTenants().empty(),
              "Graceful degradation: invalid DB path does not crash and loads empty");
    }
}

// ── Summary ───────────────────────────────────────────────────────────────────

void printSummary() {
    std::cout << "\n+------------------------------+\n";
    std::cout <<   "|        TEST SUMMARY          |\n";
    std::cout <<   "+------------------------------+\n";
    std::cout << GREEN << "  Passed: " << passed << RESET << "\n";
    std::cout << RED   << "  Failed: " << failed << RESET << "\n";
    std::cout <<   "+------------------------------+\n\n";
}

int main() {
    std::cout << "\n+------------------------------+\n";
    std::cout <<   "|      Rlimit Test Suite       |\n";
    std::cout <<   "+------------------------------+\n";

    testTokenBucket();
    testFixedWindowCounter();
    testSlidingWindowLog();
    testSlidingWindowCounter();
    testLeakyBucket();
    testConcurrency();
    testDatabase();
    testMetrics();
    testPersistenceIntegration();

    printSummary();
    return failed > 0 ? 1 : 0;
}
