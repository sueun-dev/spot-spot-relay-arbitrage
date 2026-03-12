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

set(KIMP_SIMDJSON_TARGET "")
set(KIMP_SPDLOG_TARGET "")
set(KIMP_FMT_TARGET "")
set(KIMP_YAML_CPP_TARGET "")

find_package(simdjson CONFIG QUIET)
if(TARGET simdjson::simdjson)
    set(KIMP_SIMDJSON_TARGET simdjson::simdjson)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)
    set(KIMP_SIMDJSON_TARGET PkgConfig::SIMDJSON)
endif()

find_package(spdlog CONFIG QUIET)
if(TARGET spdlog::spdlog)
    set(KIMP_SPDLOG_TARGET spdlog::spdlog)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
    set(KIMP_SPDLOG_TARGET PkgConfig::SPDLOG)
endif()

find_package(fmt CONFIG QUIET)
if(TARGET fmt::fmt)
    set(KIMP_FMT_TARGET fmt::fmt)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FMT REQUIRED IMPORTED_TARGET fmt)
    set(KIMP_FMT_TARGET PkgConfig::FMT)
endif()

find_package(yaml-cpp CONFIG QUIET)
if(TARGET yaml-cpp::yaml-cpp)
    set(KIMP_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(YAML_CPP REQUIRED IMPORTED_TARGET yaml-cpp)
    set(KIMP_YAML_CPP_TARGET PkgConfig::YAML_CPP)
endif()

# Include directories
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/jwt-cpp/include
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
        ${KIMP_SIMDJSON_TARGET}
        ${KIMP_SPDLOG_TARGET}
        ${KIMP_FMT_TARGET}
        ${KIMP_YAML_CPP_TARGET}
)

# Main executable
add_executable(kimp_bot src/main.cpp)
target_link_libraries(kimp_bot PRIVATE kimp_lib)

# Entry selection test
add_executable(kimp_test_entry tests/test_entry_selection.cpp)
target_link_libraries(kimp_test_entry PRIVATE kimp_lib)

# Regression: entry signal count must not exceed available position slots
add_executable(kimp_test_entry_capacity tests/test_entry_signal_capacity.cpp)
target_link_libraries(kimp_test_entry_capacity PRIVATE kimp_lib)

# Regression: lifecycle executor must reuse fixed workers and drain queue
add_executable(kimp_test_lifecycle_executor tests/test_lifecycle_executor.cpp)
target_link_libraries(kimp_test_lifecycle_executor PRIVATE kimp_lib)

# Regression: atomic bitset must keep deterministic set iteration/counts
add_executable(kimp_test_atomic_bitset tests/test_atomic_bitset.cpp)
target_link_libraries(kimp_test_atomic_bitset PRIVATE kimp_lib)

# Regression: bitmap-based entry selection must preserve slot semantics
add_executable(kimp_test_entry_bitmap tests/test_entry_selection_bitmap.cpp)
target_link_libraries(kimp_test_entry_bitmap PRIVATE kimp_lib)

# Stress smoke test for detached async export lifetime behavior
add_executable(kimp_test_export_async_stability tests/test_export_async_stability.cpp)
target_link_libraries(kimp_test_export_async_stability PRIVATE kimp_lib)

# Fill price parsing test (JSON parsing verification, no network)
add_executable(kimp_test_fill tests/test_fill_parsing.cpp)
target_link_libraries(kimp_test_fill PRIVATE kimp_lib)

# Section 1 test: Process startup & config loading
add_executable(kimp_test_init_s1 tests/test_init_section1.cpp)
target_link_libraries(kimp_test_init_s1 PRIVATE kimp_lib)

# Section 2 test: Logger & Directory initialization
add_executable(kimp_test_init_s2 tests/test_init_section2.cpp)
target_link_libraries(kimp_test_init_s2 PRIVATE kimp_lib)

# Sections 1-4 integration test (빡센 테스트)
add_executable(kimp_test_s1_to_s4 tests/test_sections_1_to_4.cpp)
target_link_libraries(kimp_test_s1_to_s4 PRIVATE kimp_lib)

# Sections 6-8: IO thread pool, WebSocket, common symbols & preprocessing
add_executable(kimp_test_s6_to_s8 tests/test_sections_6_to_8.cpp)
target_link_libraries(kimp_test_s6_to_s8 PRIVATE kimp_lib)

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
