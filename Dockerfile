FROM debian:12-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make libsqlite3-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN g++ -std=c++17 -O2 -pthread -I. -o rlimit_server \
    server_main.cpp \
    PolicyEngine.cpp TokenBucket.cpp LeakyBucket.cpp SlidingWindowCounter.cpp \
    SlidingWindowLog.cpp FixedWindowCounter.cpp Database.cpp StrategyFactory.cpp \
    api/JsonCodec.cpp api/RestController.cpp api/Server.cpp \
    policy/Policy.cpp policy/PolicyResolver.cpp policy/PolicyLoader.cpp \
    storage/StateStore.cpp storage/InMemoryStateStore.cpp storage/RedisStateStore.cpp \
    storage/RedisRateLimitStrategies.cpp \
    -lsqlite3

EXPOSE 8080
CMD ["./rlimit_server"]
