# Rlimit

Rlimit is a policy-driven C++17 rate-limiting engine with five algorithms, tenant-and-endpoint policy resolution, a CLI simulator, a concurrent REST service, Redis-backed distributed state, local audit persistence, and an automated test suite.

The critical paths now include realistic backend safeguards:

- all five algorithms work locally and through atomic Redis Lua scripts;
- distributed scripts use Redis server time rather than application clocks;
- tenant and endpoint both participate in the quota namespace;
- Redis keys use a shared hash tag for Cluster slot compatibility;
- Redis errors return HTTP 503 instead of looking like normal denials;
- tenant credentials and policy-administrator credentials are separate;
- HTTP work uses a bounded worker pool with framing, size, and timeout limits;
- policy parameters and JSON input are structurally validated;
- audit appends are synchronized and O(1), with bounded retention;
- CMake, CI, multi-stage Docker, health checks, and persistent Compose volumes are included.

For the complete code walkthrough, algorithms, call graphs, security analysis, and interview preparation, read TECHNICAL_ONBOARDING.md.

## Architecture

~~~mermaid
flowchart LR
    Client --> Server["Bounded HTTP worker pool"]
    Server --> Controller["RestController"]
    Controller --> Auth["Tenant/admin bearer authentication"]
    Controller --> Resolver["PolicyResolver"]
    Resolver --> Policies["policy/policies.yaml"]
    Controller --> Engine["PolicyEngine"]
    Engine --> Strategies["RateLimitStrategy"]
    Strategies --> Redis["Redis 7 / atomic Lua"]
    Controller --> Audit["Thread-safe append audit store"]
~~~

The CLI is a separate entry point. It uses the same five local strategies, restores tenant policy definitions from rlimit_store.txt, and provides simulations, audit inspection, stress tests, and benchmarks.

## Algorithms

| Algorithm | Main behavior | Time | State |
|---|---|---:|---:|
| Token Bucket | Average rate with bounded bursts | O(1) | O(1) |
| Fixed Window Counter | Simple quota per request-anchored window | O(1) | O(1) |
| Sliding Window Log | Exact rolling-window count | O(1) amortized | O(limit) |
| Sliding Window Counter | Weighted two-window approximation | O(1) | O(1) |
| Leaky Bucket | Admission into a virtual continuously draining queue | O(1) | O(1) |

All five are distributed in the REST path. Sliding Window Log uses an atomic Redis sorted-set script with unique sequence members.

## Policy behavior

Policies are loaded from policy/policies.yaml, sorted by descending priority, and resolved using exact optional tenant/endpoint match fields. Equal priorities retain file order. Missing match fields are wildcards.

The REST quota key includes length-prefixed tenant and endpoint values plus normalized algorithm parameters. Identical limits on two endpoints therefore remain isolated. Redis receives only a stable hash-tagged identifier derived from this key.

Example:

~~~yaml
- priority: 100
  match:
    endpoint: "/login"
  algorithm: TokenBucket
  capacity: 10
  refillRate: 1
~~~

Parameter validation requires positive capacity/window/integer limits, non-negative rates, finite values, only the parameters supported by the algorithm, and bounded tenant/absolute endpoint strings.

## Build

### CMake

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~

Targets are Rlimit (CLI), rlimit_server (REST), and tester.

On Windows, use a modern toolchain with C++ threading support. MSYS2 UCRT64 GCC 15.2 was verified; an old MinGW GCC 6.3 installation on PATH was not thread-capable.

### Direct Linux compile

~~~bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -I. -o rlimit_server \
  server_main.cpp PolicyEngine.cpp TokenBucket.cpp LeakyBucket.cpp \
  SlidingWindowCounter.cpp SlidingWindowLog.cpp FixedWindowCounter.cpp \
  Database.cpp StrategyFactory.cpp api/JsonCodec.cpp api/RestController.cpp \
  api/Server.cpp policy/Policy.cpp policy/PolicyResolver.cpp \
  policy/PolicyLoader.cpp storage/StateStore.cpp \
  storage/InMemoryStateStore.cpp storage/RedisStateStore.cpp \
  storage/RedisRateLimitStrategies.cpp
~~~

Windows also requires -lws2_32.

## Secure runtime configuration

The REST service fails startup unless Redis, audit storage, and credentials are available.

| Variable | Default | Requirement |
|---|---|---|
| HTTP_PORT | 8080 | 1-65535 |
| HTTP_WORKERS | 4 | 1-256 |
| HTTP_MAX_QUEUE | 256 | Positive |
| REDIS_HOST | localhost | Redis hostname |
| REDIS_PORT | 6379 | 1-65535 |
| REDIS_PASSWORD | none | Required, at least 16 characters |
| ADMIN_API_KEY | none | Required, at least 16 characters |
| TENANT_API_KEYS | none | Required tenant=token;tenant=token map |
| POLICY_FILE | policy/policies.yaml | Readable and writable |
| AUDIT_STORE_PATH | rlimit_store.txt | Writable |

Example PowerShell:

~~~powershell
$env:REDIS_PASSWORD = "replace-with-a-long-random-password"
$env:ADMIN_API_KEY = "replace-with-a-long-random-admin-token"
$env:TENANT_API_KEYS = "tenant-a=replace-with-a-long-tenant-token"
.\rlimit_server.exe
~~~

Generate real random values. Do not commit .env; it is ignored.

## Docker Compose

~~~bash
cp .env.example .env
# Replace every placeholder in .env.
docker compose up --build
~~~

Compose keeps Redis off the host network, enables password authentication and append-only persistence, waits for Redis health, and persists Redis, audit, and writable policy data in named volumes. The image seeds `/data/policies.yaml`, runs Rlimit as an unprivileged user, and checks `/health/ready`.

## REST API

### Evaluate

~~~bash
curl -X POST http://localhost:8080/evaluate \
  -H "Authorization: Bearer <tenant-a-token>" \
  -H "Content-Type: application/json" \
  -d '{"tenant":"tenant-a","endpoint":"/login"}'
~~~

~~~json
{
  "allowed": true,
  "result": "ALLOW",
  "tenant": "tenant-a",
  "endpoint": "/login",
  "policyKey": "8:tenant-a|6:/login|TokenBucket|capacity=10|refill=1",
  "algorithm": "TokenBucket",
  "state": "tokens=9/10 refill=1/s"
}
~~~

The token is mapped to the named tenant. Missing authentication returns 401; wrong or cross-tenant credentials return 403. A normal limiter denial is HTTP 200 with allowed:false. A Redis/state-store failure is HTTP 503 and is not audited as a normal denial.

### Policies

~~~bash
curl http://localhost:8080/policies \
  -H "Authorization: Bearer <admin-token>"

curl -X POST http://localhost:8080/policies \
  -H "Authorization: Bearer <admin-token>" \
  -H "Content-Type: application/json" \
  -d '{"priority":90,"match":{"endpoint":"/pay"},"algorithm":"TokenBucket","params":{"capacity":20,"refillRate":2}}'
~~~

Policy writes use a temporary file and atomic replacement, then update the thread-safe resolver. There is no update/delete route yet; configuration management can replace YAML and restart, or a future API can add stable policy IDs.

### Health and metrics

~~~text
GET /health/live     public process liveness
GET /health/ready    public Redis + audit readiness
GET /metrics         admin-authenticated JSON counters
~~~

Metrics report evaluation, allowed, denied, and dependency-error counts.

## HTTP safeguards

The dependency-free server enforces a configurable bounded worker pool/queue, 5-second client deadlines, 16 KiB headers, 1 MiB bodies, strict Content-Length, rejection of duplicate/framing headers, structural JSON validation, application/json checking, full response writes, and Connection: close.

For an internet-facing production deployment, place it behind a hardened TLS reverse proxy or replace the educational socket transport with a maintained HTTP library.

## Redis correctness

Each worker reuses one authenticated Redis connection. Ambiguous network failures return 503 and discard the connection; commands are not blindly retried because the script may already have committed.

Lua scripts use Redis TIME, atomically update all state, share a {rl:<hash>} Cluster slot, and refresh correct TTLs. Zero-rate Token/Leaky state is persistent because expiration would incorrectly restore capacity.

Redis TLS and managed HA/Cluster discovery are deployment integrations not implemented by the small native RESP client. Compose keeps Redis private and authenticated; production should use a protected network or TLS-capable managed/client integration.

## Audit persistence

Database is a legacy class name for a percent-escaped local text store. Request rows append in O(1), access is mutex-protected, history is capped at 100,000 records, tenant/compaction rewrites use temporary-file replacement, and CLI/REST timestamps are Unix epoch milliseconds.

It is suitable for local or single-process deployments. Multi-process durable analytics should use a real append/batch sink such as PostgreSQL, Kafka, or an observability pipeline.

## Tests

The current suite reports:

~~~text
Passed: 85
Failed: 0
~~~

Checks cover all algorithms, concurrency, policies, strict and unambiguous JSON, controllers, tenant/admin authentication, endpoint isolation, distributed script selection, Redis time/TTL/hash tags, dependency errors, state stores, concurrent and malformed audit handling, persistence, and metrics.

Real Redis and live-socket integration should additionally run in deployment CI. Repository CI currently performs the dependency-free build and test suite.

## Remaining boundaries

The concrete defects found during onboarding were fixed. Broader production integrations remain explicit boundaries:

- TLS termination and enterprise identity/JWT/mTLS integration;
- managed Redis HA/Cluster discovery and TLS;
- multi-process guaranteed audit delivery;
- graceful draining coordinated with an orchestrator;
- end-to-end load, chaos, and real-Redis integration environments.

These are not claimed as implemented features.
