cmake_minimum_required(VERSION 3.10)

project(Rlimit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SQLite3 REQUIRED)

set(RLIMIT_ALGORITHMS
    PolicyEngine.cpp
    TokenBucket.cpp
    LeakyBucket.cpp
    SlidingWindowCounter.cpp
    SlidingWindowLog.cpp
    FixedWindowCounter.cpp
)

set(RLIMIT_POLICY
    policy/Policy.cpp
    policy/PolicyResolver.cpp
    policy/PolicyLoader.cpp
)

set(RLIMIT_STORAGE
    storage/StateStore.cpp
    storage/InMemoryStateStore.cpp
    storage/RedisStateStore.cpp
    storage/RedisRateLimitStrategies.cpp
)

set(RLIMIT_API
    api/JsonCodec.cpp
    api/RestController.cpp
    api/Server.cpp
)

set(RLIMIT_CORE
    ${RLIMIT_ALGORITHMS}
    ${RLIMIT_POLICY}
    ${RLIMIT_STORAGE}
    StrategyFactory.cpp
)

add_executable(rlimit_cli
    main.cpp
    Database.cpp
    Metrics.cpp
    ${RLIMIT_ALGORITHMS}
)

add_executable(rlimit_server
    server_main.cpp
    Database.cpp
    ${RLIMIT_CORE}
    ${RLIMIT_API}
)
target_link_libraries(rlimit_server PRIVATE ${SQLite3_LIBRARIES})

add_executable(rlimit_tester
    tester.cpp
    Database.cpp
    Metrics.cpp
    ${RLIMIT_CORE}
    ${RLIMIT_API}
)
target_link_libraries(rlimit_tester PRIVATE ${SQLite3_LIBRARIES})

if(WIN32)
    target_link_libraries(rlimit_server PRIVATE ws2_32)
    target_link_libraries(rlimit_tester PRIVATE ws2_32)
endif()

target_link_libraries(rlimit_cli PRIVATE ${SQLite3_LIBRARIES})

target_include_directories(rlimit_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLite3_INCLUDE_DIRS})
target_include_directories(rlimit_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLite3_INCLUDE_DIRS})
target_include_directories(rlimit_tester PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLite3_INCLUDE_DIRS})
