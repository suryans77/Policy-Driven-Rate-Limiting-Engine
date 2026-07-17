FROM debian:12-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -I. -o rlimit_server \
    server_main.cpp \
    PolicyEngine.cpp TokenBucket.cpp LeakyBucket.cpp SlidingWindowCounter.cpp \
    SlidingWindowLog.cpp FixedWindowCounter.cpp Database.cpp StrategyFactory.cpp \
    api/JsonCodec.cpp api/RestController.cpp api/Server.cpp \
    policy/Policy.cpp policy/PolicyResolver.cpp policy/PolicyLoader.cpp \
    storage/StateStore.cpp storage/InMemoryStateStore.cpp storage/RedisStateStore.cpp \
    storage/RedisRateLimitStrategies.cpp

FROM debian:12-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system rlimit \
    && useradd --system --gid rlimit --home-dir /app rlimit

WORKDIR /app
COPY --from=builder /src/rlimit_server /app/rlimit_server
COPY --chown=rlimit:rlimit policy /app/policy
RUN mkdir -p /data && chown rlimit:rlimit /data /app/policy
COPY --chown=rlimit:rlimit policy/policies.yaml /data/policies.yaml

USER rlimit
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=3 \
    CMD curl --fail --silent http://127.0.0.1:8080/health/ready || exit 1

CMD ["./rlimit_server"]
