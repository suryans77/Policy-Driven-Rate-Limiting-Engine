#include "Metrics.h"
#include "TokenBucket.h"
#include "FixedWindowCounter.h"
#include "SlidingWindowLog.h"
#include "SlidingWindowCounter.h"
#include "LeakyBucket.h"
#include "PolicyEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <thread>

namespace {
using Clock = std::chrono::high_resolution_clock;

ThroughputResult measure(const std::string& name, std::unique_ptr<RateLimitStrategy> strategy) {
    const int requests = 100000;
    auto start = Clock::now();
    for (int i = 0; i < requests; ++i) {
        strategy->allowRequest();
    }
    auto end = Clock::now();

    auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double elapsedSec = std::max(1.0, static_cast<double>(elapsedUs)) / 1000000.0;
    long long elapsedMs = std::max(1LL, std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    return {name, elapsedMs, requests / elapsedSec};
}
}

std::vector<ThroughputResult> Metrics::runThroughputBenchmark() {
    std::vector<ThroughputResult> results;
    results.push_back(measure("TokenBucket", std::make_unique<TokenBucket>(100000.0, 100000.0)));
    results.push_back(measure("FixedWindowCounter", std::make_unique<FixedWindowCounter>(100000, 60.0)));
    results.push_back(measure("SlidingWindowCounter", std::make_unique<SlidingWindowCounter>(100000, 60.0)));
    results.push_back(measure("LeakyBucket", std::make_unique<LeakyBucket>(100000.0, 100000.0)));
    results.push_back(measure("SlidingWindowLog", std::make_unique<SlidingWindowLog>(100000, 60.0)));
    return results;
}

ConcurrencyResult Metrics::runConcurrencyVariance() {
    const int runs = 20;
    const int limit = 1000;
    const int perThread = 1000;

    int minOver = std::numeric_limits<int>::max();
    int maxOver = 0;
    int totalOver = 0;
    int races = 0;

    for (int run = 0; run < runs; ++run) {
        PolicyEngine engine;
        engine.setPolicy("stress", std::make_unique<TokenBucket>(limit, 0.0));
        std::atomic<int> allowed{0};

        auto worker = [&]() {
            for (int i = 0; i < perThread; ++i) {
                if (engine.evaluateUnsafe("stress")) ++allowed;
            }
        };

        std::thread t1(worker), t2(worker);
        t1.join();
        t2.join();

        int over = std::max(0, allowed.load() - limit);
        minOver = std::min(minOver, over);
        maxOver = std::max(maxOver, over);
        totalOver += over;
        if (over > 0) ++races;
    }

    return {minOver, maxOver, totalOver / static_cast<double>(runs), races};
}

AccuracyResult Metrics::runAccuracyComparison() {
    const int runs = 5;
    const int requests = 100;
    double totalError = 0.0;
    int lastLogAllows = 0;
    int lastCounterAllows = 0;

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> delayMs(0, 50);

    for (int run = 0; run < runs; ++run) {
        SlidingWindowLog exact(20, 10.0);
        SlidingWindowCounter approx(20, 10.0);
        int logAllows = 0;
        int counterAllows = 0;

        for (int i = 0; i < requests; ++i) {
            if (exact.allowRequest()) ++logAllows;
            if (approx.allowRequest()) ++counterAllows;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs(rng)));
        }

        double error = 0.0;
        if (logAllows > 0) {
            error = std::abs(counterAllows - logAllows) / static_cast<double>(logAllows) * 100.0;
        }
        totalError += error;
        lastLogAllows = logAllows;
        lastCounterAllows = counterAllows;
    }

    return {totalError / runs, lastLogAllows, lastCounterAllows};
}
