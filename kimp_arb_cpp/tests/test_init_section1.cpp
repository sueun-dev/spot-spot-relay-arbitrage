/**
 * Test: Process Startup & Config Loading (Section 1)
 *
 * main.cpp 섹션 1 검증:
 * 1. expand_env - ${VAR} 환경변수 확장, 없으면 빈 문자열 + 에러
 * 2. load_config - YAML 로드, 실패 시 nullopt
 * 3. load_config - exchanges 검증 (api_key, secret_key 필수)
 * 4. CLI 파싱 로직 검증 (-c, -m, -h, unknown)
 */

#include "kimp/core/types.hpp"
#include "kimp/core/config.hpp"

#include <yaml-cpp/yaml.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <fmt/format.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cout << "  [TEST] " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; ++tests_passed;

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; ++tests_failed;

#define ASSERT_TRUE(cond, msg) \
    if (!(cond)) { FAIL(msg); return; }

#define ASSERT_EQ(a, b, msg) \
    if ((a) != (b)) { FAIL(fmt::format("{}: {} != {}", msg, a, b)); return; }

// ===================== expand_env 복제 (main.cpp 로직) =====================

std::string expand_env(const std::string& val) {
    if (val.size() > 3 && val[0] == '$' && val[1] == '{' && val.back() == '}') {
        std::string env_name = val.substr(2, val.size() - 3);
        const char* env_val = std::getenv(env_name.c_str());
        if (!env_val) {
            std::cerr << "FATAL: Environment variable '" << env_name
                      << "' is not set" << std::endl;
            return {};
        }
        return env_val;
    }
    return val;
}

// ===================== load_config 복제 (main.cpp 로직) =====================

std::optional<kimp::RuntimeConfig> load_config(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "Config file not found: " << path << std::endl;
        return std::nullopt;
    }

    kimp::RuntimeConfig config;

    try {
        YAML::Node yaml = YAML::LoadFile(path);

        if (yaml["logging"]) {
            auto l = yaml["logging"];
            if (l["level"]) config.log_level = l["level"].as<std::string>();
            if (l["file"]) config.log_file = l["file"].as<std::string>();
        }

        if (yaml["threading"]) {
            auto t = yaml["threading"];
            if (t["io_threads"]) config.io_threads = t["io_threads"].as<int>();
        }

        if (!yaml["exchanges"]) {
            std::cerr << "No 'exchanges' section in config" << std::endl;
            return std::nullopt;
        }

        auto load_exchange = [&](const std::string& name, kimp::Exchange ex) -> bool {
            if (!yaml["exchanges"][name]) {
                std::cerr << "Exchange '" << name << "' not found in config" << std::endl;
                return false;
            }

            auto e = yaml["exchanges"][name];
            kimp::ExchangeCredentials creds;
            creds.enabled = e["enabled"] ? e["enabled"].as<bool>() : true;

            if (!creds.enabled) {
                config.exchanges[ex] = std::move(creds);
                return true;
            }

            if (e["ws_endpoint"]) creds.ws_endpoint = e["ws_endpoint"].as<std::string>();
            if (e["rest_endpoint"]) creds.rest_endpoint = e["rest_endpoint"].as<std::string>();
            if (e["api_key"]) creds.api_key = expand_env(e["api_key"].as<std::string>());
            if (e["secret_key"]) creds.secret_key = expand_env(e["secret_key"].as<std::string>());

            if (creds.api_key.empty() || creds.secret_key.empty()) {
                std::cerr << "Exchange '" << name
                          << "': api_key or secret_key is missing" << std::endl;
                return false;
            }

            config.exchanges[ex] = std::move(creds);
            return true;
        };

        if (!load_exchange("bithumb", kimp::Exchange::Bithumb)) return std::nullopt;
        if (!load_exchange("bybit", kimp::Exchange::Bybit)) return std::nullopt;

    } catch (const YAML::Exception& e) {
        std::cerr << "Failed to parse config: " << e.what() << std::endl;
        return std::nullopt;
    }

    return config;
}

// ===================== expand_env Tests =====================

void test_expand_env_set() {
    TEST("expand_env with set variable") {
        setenv("KIMP_TEST_VAR", "test_value_123", 1);
        std::string result = expand_env("${KIMP_TEST_VAR}");
        ASSERT_EQ(result, std::string("test_value_123"), "should expand");
        unsetenv("KIMP_TEST_VAR");
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_expand_env_unset() {
    TEST("expand_env with unset variable returns empty") {
        unsetenv("KIMP_NONEXISTENT_VAR_12345");
        // Capture stderr to avoid noise
        std::string result = expand_env("${KIMP_NONEXISTENT_VAR_12345}");
        ASSERT_TRUE(result.empty(), "should return empty string");
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_expand_env_passthrough() {
    TEST("expand_env passthrough non-${} strings") {
        ASSERT_EQ(expand_env("plain_string"), std::string("plain_string"), "plain");
        ASSERT_EQ(expand_env("$NOT_BRACED"), std::string("$NOT_BRACED"), "no braces");
        ASSERT_EQ(expand_env("${"), std::string("${"), "incomplete 1");
        ASSERT_EQ(expand_env("${}"), std::string("${}"), "empty name");
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

// ===================== load_config Tests =====================

void test_load_config_file_not_found() {
    TEST("load_config file not found returns nullopt") {
        auto result = load_config("/nonexistent/path/config.yaml");
        ASSERT_TRUE(!result.has_value(), "should be nullopt");
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_load_config_no_exchanges_section() {
    TEST("load_config missing exchanges section") {
        std::string tmp = "/tmp/kimp_test_s1_no_ex.yaml";
        {
            std::ofstream f(tmp);
            f << "logging:\n  level: debug\n";
        }
        auto result = load_config(tmp);
        ASSERT_TRUE(!result.has_value(), "should fail without exchanges");
        std::filesystem::remove(tmp);
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_load_config_missing_exchange() {
    TEST("load_config missing required exchange") {
        std::string tmp = "/tmp/kimp_test_s1_miss_ex.yaml";
        {
            std::ofstream f(tmp);
            f << "exchanges:\n"
              << "  bithumb:\n"
              << "    api_key: key1\n"
              << "    secret_key: secret1\n";
            // bybit is missing
        }
        auto result = load_config(tmp);
        ASSERT_TRUE(!result.has_value(), "should fail without bybit");
        std::filesystem::remove(tmp);
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_load_config_empty_api_key() {
    TEST("load_config empty api_key fails") {
        std::string tmp = "/tmp/kimp_test_s1_empty_key.yaml";
        {
            std::ofstream f(tmp);
            f << "exchanges:\n"
              << "  bithumb:\n"
              << "    api_key: ''\n"
              << "    secret_key: secret1\n"
              << "  bybit:\n"
              << "    api_key: key2\n"
              << "    secret_key: secret2\n";
        }
        auto result = load_config(tmp);
        ASSERT_TRUE(!result.has_value(), "should fail with empty api_key");
        std::filesystem::remove(tmp);
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_load_config_disabled_exchange() {
    TEST("load_config disabled exchange skips key validation") {
        std::string tmp = "/tmp/kimp_test_s1_disabled.yaml";
        {
            std::ofstream f(tmp);
            f << "exchanges:\n"
              << "  bithumb:\n"
              << "    enabled: true\n"
              << "    api_key: key1\n"
              << "    secret_key: secret1\n"
              << "  bybit:\n"
              << "    enabled: false\n";
            // bybit disabled, no keys needed
        }
        auto result = load_config(tmp);
        ASSERT_TRUE(result.has_value(), "should succeed with disabled bybit");
        ASSERT_TRUE(!result->exchanges[kimp::Exchange::Bybit].enabled, "bybit disabled");
        std::filesystem::remove(tmp);
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_load_config_success() {
    TEST("load_config full success") {
        std::string tmp = "/tmp/kimp_test_s1_success.yaml";
        setenv("KIMP_TEST_SECRET", "env_secret_value", 1);
        {
            std::ofstream f(tmp);
            f << "logging:\n"
              << "  level: warn\n"
              << "  file: /tmp/test.log\n"
              << "threading:\n"
              << "  io_threads: 8\n"
              << "exchanges:\n"
              << "  bithumb:\n"
              << "    api_key: bithumb_key\n"
              << "    secret_key: ${KIMP_TEST_SECRET}\n"
              << "  bybit:\n"
              << "    api_key: bybit_key\n"
              << "    secret_key: bybit_secret\n";
        }
        auto result = load_config(tmp);
        ASSERT_TRUE(result.has_value(), "should succeed");

        // Verify logging
        ASSERT_EQ(result->log_level, std::string("warn"), "log_level");
        ASSERT_EQ(result->log_file, std::string("/tmp/test.log"), "log_file");

        // Verify threading
        ASSERT_EQ(result->io_threads, 8, "io_threads");

        // Verify exchanges
        ASSERT_EQ(result->exchanges[kimp::Exchange::Bithumb].api_key,
                  std::string("bithumb_key"), "bithumb api_key");
        ASSERT_EQ(result->exchanges[kimp::Exchange::Bithumb].secret_key,
                  std::string("env_secret_value"), "bithumb secret_key from env");
        ASSERT_EQ(result->exchanges[kimp::Exchange::Bybit].api_key,
                  std::string("bybit_key"), "bybit api_key");

        unsetenv("KIMP_TEST_SECRET");
        std::filesystem::remove(tmp);
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

void test_load_config_invalid_yaml() {
    TEST("load_config invalid YAML syntax") {
        std::string tmp = "/tmp/kimp_test_s1_invalid.yaml";
        {
            std::ofstream f(tmp);
            f << "this is not valid: yaml: syntax:\n  - [broken";
        }
        auto result = load_config(tmp);
        ASSERT_TRUE(!result.has_value(), "should fail on invalid YAML");
        std::filesystem::remove(tmp);
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

// ===================== RuntimeConfig Defaults =====================

void test_runtime_config_defaults() {
    TEST("RuntimeConfig has correct defaults") {
        kimp::RuntimeConfig cfg;
        ASSERT_EQ(cfg.log_level, std::string("info"), "default log_level");
        ASSERT_EQ(cfg.log_file, std::string("logs/kimp_bot.log"), "default log_file");
        ASSERT_EQ(cfg.log_max_size_mb, 100, "default log_max_size_mb");
        ASSERT_EQ(cfg.log_max_files, 10, "default log_max_files");
        ASSERT_EQ(cfg.io_threads, 4, "default io_threads");
        PASS();
    } catch (const std::exception& e) { FAIL(e.what()); }
}

// ===================== Main =====================

int main() {
    std::cout << "\n=== Section 1: Process Startup & Config Loading Tests ===\n\n";

    // expand_env tests
    std::cout << "--- Environment Variable Expansion ---\n";
    test_expand_env_set();
    test_expand_env_unset();
    test_expand_env_passthrough();

    // load_config tests
    std::cout << "\n--- Config Loading ---\n";
    test_load_config_file_not_found();
    test_load_config_no_exchanges_section();
    test_load_config_missing_exchange();
    test_load_config_empty_api_key();
    test_load_config_disabled_exchange();
    test_load_config_success();
    test_load_config_invalid_yaml();

    // Defaults
    std::cout << "\n--- RuntimeConfig Defaults ---\n";
    test_runtime_config_defaults();

    // Summary
    std::cout << "\n=== Results ===\n";
    std::cout << fmt::format("  Passed: {}\n", tests_passed);
    std::cout << fmt::format("  Failed: {}\n", tests_failed);
    std::cout << fmt::format("  Total:  {}\n", tests_passed + tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
