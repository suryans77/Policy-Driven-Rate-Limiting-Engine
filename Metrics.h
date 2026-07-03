#pragma once

#include <string>
#include <vector>

struct ThroughputResult {
    std::string algorithm;
    long long totalTimeMs;
    double requestsPerSec;
};

struct ConcurrencyResult {
    int minOverAllows;
    int maxOverAllows;
    double avgOverAllows;
    int racesFired;
};

struct AccuracyResult {
    double avgErrorPercent;
    int logAllows;
    int counterAllows;
};

class Metrics {
public:
    std::vector<ThroughputResult> runThroughputBenchmark();
    ConcurrencyResult runConcurrencyVariance();
    AccuracyResult runAccuracyComparison();
};
