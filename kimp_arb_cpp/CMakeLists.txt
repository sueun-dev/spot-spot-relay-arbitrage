cmake_minimum_required(VERSION 3.20)
project(kimp_arb_cpp VERSION 1.0.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native")
endif()

# macOS/Homebrew paths
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/usr/local")
    set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
endif()

# Find packages
find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)

# Find other packages via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)
pkg_check_modules(YAML_CPP REQUIRED IMPORTED_TARGET yaml-cpp)

# Include directories
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp/include
    ${Boost_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${SIMDJSON_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS}
    ${FMT_INCLUDE_DIRS}
    ${YAML_CPP_INCLUDE_DIRS}
)

# Collect source files
file(GLOB_RECURSE LIB_SOURCES
    "src/core/*.cpp"
    "src/memory/*.cpp"
    "src/network/*.cpp"
    "src/exchange/*.cpp"
    "src/data/*.cpp"
    "src/strategy/*.cpp"
    "src/execution/*.cpp"
    "src/utils/*.cpp"
)

# Create library for testing
add_library(kimp_lib STATIC ${LIB_SOURCES})
target_include_directories(kimp_lib PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp/include
)
target_link_libraries(kimp_lib
    PUBLIC
        Boost::json
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        PkgConfig::SIMDJSON
        PkgConfig::SPDLOG
        PkgConfig::FMT
        PkgConfig::YAML_CPP
)

# Main executable
add_executable(kimp_bot src/main.cpp)
target_link_libraries(kimp_bot PRIVATE kimp_lib)

# Diagnostic trade executable (separate from main bot)
add_executable(kimp_diag_trade tests/integration/diag_trade.cpp)
target_link_libraries(kimp_diag_trade PRIVATE kimp_lib)

# Gap accuracy diagnostic executable (10-minute false-positive check)
add_executable(kimp_diag_gap tests/integration/diag_gap_accuracy.cpp)
target_link_libraries(kimp_diag_gap PRIVATE kimp_lib)

# Entry selection test
add_executable(kimp_test_entry tests/test_entry_selection.cpp)
target_link_libraries(kimp_test_entry PRIVATE kimp_lib)

# Live entry selection test (fetches real-time data from APIs)
add_executable(kimp_test_live_entry tests/test_live_entry_selection.cpp)
target_link_libraries(kimp_test_live_entry PRIVATE kimp_lib)

# Price verification test (10-minute live bid/ask comparison)
add_executable(kimp_test_price tests/test_price_verify.cpp)
target_link_libraries(kimp_test_price PRIVATE kimp_lib)

# Fill price parsing test (JSON parsing verification, no network)
add_executable(kimp_test_fill tests/test_fill_parsing.cpp)
target_link_libraries(kimp_test_fill PRIVATE kimp_lib)

# Symbol matching test (live API - verifies both exchanges return correct symbols)
add_executable(kimp_test_symbols tests/test_symbol_matching.cpp)
target_link_libraries(kimp_test_symbols PRIVATE kimp_lib)

# Market data pipeline test (ticker, orderbook BBO, USDT/KRW, premium)
add_executable(kimp_test_market tests/test_market_data.cpp)
target_link_libraries(kimp_test_market PRIVATE kimp_lib)

# E2E pipeline test (#1 거래소 연결 → #2 시세 수신 → #3 프리미엄 계산)
add_executable(kimp_test_e2e tests/test_e2e_pipeline.cpp)
target_link_libraries(kimp_test_e2e PRIVATE kimp_lib)

# Section 1 test: Process startup & config loading
add_executable(kimp_test_init_s1 tests/test_init_section1.cpp)
target_link_libraries(kimp_test_init_s1 PRIVATE kimp_lib)

# Section 2 test: Logger & Directory initialization
add_executable(kimp_test_init_s2 tests/test_init_section2.cpp)
target_link_libraries(kimp_test_init_s2 PRIVATE kimp_lib)

# Sections 1-4 integration test (빡센 테스트)
add_executable(kimp_test_s1_to_s4 tests/test_sections_1_to_4.cpp)
target_link_libraries(kimp_test_s1_to_s4 PRIVATE kimp_lib)

# Section 5: Entry/Exit callback & hedge accuracy test
add_executable(kimp_test_s5_hedge tests/test_section5_hedge.cpp)
target_link_libraries(kimp_test_s5_hedge PRIVATE kimp_lib)

# Sections 6-8: IO thread pool, WebSocket, common symbols & preprocessing
add_executable(kimp_test_s6_to_s8 tests/test_sections_6_to_8.cpp)
target_link_libraries(kimp_test_s6_to_s8 PRIVATE kimp_lib)

# Live hedge accuracy test (REAL TRADES: $25 x 3 entry + split exit)
add_executable(kimp_test_live_hedge tests/test_live_hedge_accuracy.cpp)
target_link_libraries(kimp_test_live_hedge PRIVATE kimp_lib)

# Copy config and trade_logs directories to build dir so bot runs from build/
add_custom_command(TARGET kimp_bot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/config ${CMAKE_BINARY_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/trade_logs
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/logs
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/data
    COMMENT "Copying config/ and creating logs/trade_logs/data dirs in build directory"
)

# Install
install(TARGETS kimp_bot RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/kimp)

# Print configuration summary
message(STATUS "")
message(STATUS "=== KIMP Arbitrage Bot Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "=========================================")
message(STATUS "")
