cmake_minimum_required(VERSION 3.20)
project(HighPerformanceHTTPServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g3 -fsanitize=address,undefined -fno-omit-frame-pointer")

add_compile_options(
  -Wall -Wextra -Wpedantic -Wshadow
  -Wno-unused-parameter
  $<$<CONFIG:Release>:-fvisibility=hidden>
)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

find_package(spdlog QUIET)
find_package(fmt QUIET)
find_package(nlohmann_json QUIET)

if(NOT spdlog_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.12.0
  )
  FetchContent_MakeAvailable(spdlog)
endif()

if(NOT fmt_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
  )
  FetchContent_MakeAvailable(fmt)
endif()

if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(SERVER_SOURCES
  src/main.cpp
  src/server.cpp
  src/connection.cpp
  src/router.cpp
  src/http_parser.cpp
  src/thread_pool.cpp
  src/middleware.cpp
  src/config_loader.cpp
  src/logger.cpp
  src/static_file_server.cpp
  src/tls_context.cpp
)

add_executable(http_server ${SERVER_SOURCES})

target_include_directories(http_server PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(http_server PRIVATE
  Threads::Threads
  OpenSSL::SSL
  OpenSSL::Crypto
  ZLIB::ZLIB
  spdlog::spdlog
  fmt::fmt
  nlohmann_json::nlohmann_json
)

target_compile_features(http_server PRIVATE cxx_std_20)

add_executable(benchmark benchmarks/benchmark.cpp)
target_include_directories(benchmark PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(benchmark PRIVATE Threads::Threads fmt::fmt spdlog::spdlog)

enable_testing()
find_package(GTest QUIET)

if(GTest_FOUND)
  add_executable(integration_tests tests/integration_tests.cpp tests/http_tests.cpp)
  target_include_directories(integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(integration_tests PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    spdlog::spdlog
    fmt::fmt
    nlohmann_json::nlohmann_json
    GTest::GTest
    GTest::Main
  )
  add_test(NAME IntegrationTests COMMAND integration_tests)
endif()

install(TARGETS http_server RUNTIME DESTINATION bin)
install(FILES config/server_config.json DESTINATION etc/http_server)
