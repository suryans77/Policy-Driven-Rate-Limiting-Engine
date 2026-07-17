cmake_minimum_required(VERSION 3.16)
project(Rlimit VERSION 0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(RLIMIT_CORE_SOURCES
    PolicyEngine.cpp TokenBucket.cpp LeakyBucket.cpp SlidingWindowCounter.cpp
    SlidingWindowLog.cpp FixedWindowCounter.cpp Database.cpp Metrics.cpp
    StrategyFactory.cpp api/JsonCodec.cpp api/RestController.cpp api/Server.cpp
    policy/Policy.cpp policy/PolicyResolver.cpp policy/PolicyLoader.cpp
    storage/StateStore.cpp storage/InMemoryStateStore.cpp
    storage/RedisStateStore.cpp storage/RedisRateLimitStrategies.cpp)

add_library(rlimit_core STATIC ${RLIMIT_CORE_SOURCES})
target_include_directories(rlimit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rlimit_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(rlimit_core PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(rlimit_core PRIVATE /W4)
else()
    target_compile_options(rlimit_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(Rlimit main.cpp)
target_link_libraries(Rlimit PRIVATE rlimit_core)

add_executable(rlimit_server server_main.cpp)
target_link_libraries(rlimit_server PRIVATE rlimit_core)

add_executable(tester tester.cpp)
target_link_libraries(tester PRIVATE rlimit_core)

enable_testing()
add_test(NAME rlimit_tests COMMAND tester)
set_tests_properties(rlimit_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
