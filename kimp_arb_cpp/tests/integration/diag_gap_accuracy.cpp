#include "kimp/core/config.hpp"
#include "kimp/core/logger.hpp"
#include "kimp/core/optimization.hpp"
#include "kimp/core/types.hpp"
#include "kimp/exchange/exchange_base.hpp"

#include <boost/asio.hpp>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace net = boost::asio;

namespace {

struct Quote {
    double bid{0.0};
    double ask{0.0};
    uint64_t ts_ms{0};
};

struct Candidate {
    std::string base;
    double kr_bid{0.0};
    double kr_ask{0.0};
    double by_bid{0.0};
    double by_ask{0.0};
    double exit_pm{0.0};
    double spread_pm{0.0};
    double initial_entry_pm{0.0};
    double verify_entry_pm{0.0};
    double delta_pm{0.0};
    int64_t verify_latency_ms{0};
    bool verified{false};
    bool false_positive{false};
    bool skipped_latency{false};
};

struct Stats {
    int rounds{0};
    int fetch_fail_rounds{0};
    int total_candidates{0};
    int verified_candidates{0};
    int false_positives{0};
    int skipped_due_latency{0};
    int verify_failures{0};
    double worst_delta_abs{0.0};
    std::string worst_delta_symbol;
};

std::atomic<bool> g_stop{false};

void signal_handler(int) {
    g_stop.store(true, std::memory_order_release);
}

uint64_t steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t parse_timestamp_ms(simdjson::ondemand::value value) {
    std::string_view raw = value.raw_json_token();
    if (raw.empty()) {
        return 0;
    }

    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw = raw.substr(1, raw.size() - 2);
    }

    double parsed = kimp::opt::fast_stod(raw);
    return parsed > 0.0 ? static_cast<uint64_t>(parsed) : 0;
}

std::string extract_host(std::string url) {
    if (url.rfind("https://", 0) == 0) {
        url = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        url = url.substr(7);
    }
    auto slash = url.find('/');
    if (slash != std::string::npos) {
        url = url.substr(0, slash);
    }
    auto colon = url.find(':');
    if (colon != std::string::npos) {
        url = url.substr(0, colon);
    }
    return url;
}

std::string to_upper(std::string value) {
    for (auto& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::unordered_set<std::string> parse_focus_csv(const std::string& csv) {
    std::unordered_set<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',') {
            if (!cur.empty()) out.insert(to_upper(cur));
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.insert(to_upper(cur));
    return out;
}

double entry_premium(double korean_ask, double foreign_bid, double usdt_krw) {
    if (korean_ask <= 0.0 || foreign_bid <= 0.0 || usdt_krw <= 0.0) return 0.0;
    double foreign_krw = foreign_bid * usdt_krw;
    if (foreign_krw <= 0.0) return 0.0;
    return ((korean_ask - foreign_krw) / foreign_krw) * 100.0;
}

double exit_premium(double korean_bid, double foreign_ask, double usdt_krw) {
    if (korean_bid <= 0.0 || foreign_ask <= 0.0 || usdt_krw <= 0.0) return 0.0;
    double foreign_krw = foreign_ask * usdt_krw;
    if (foreign_krw <= 0.0) return 0.0;
    return ((korean_bid - foreign_krw) / foreign_krw) * 100.0;
}

bool fetch_bithumb_all_orderbook(kimp::exchange::RestClient& client,
                                 std::unordered_map<std::string, Quote>& quotes,
                                 double& usdt_krw) {
    quotes.clear();
    usdt_krw = 0.0;

    auto resp = client.get("/public/orderbook/ALL_KRW?count=1");
    if (!resp.success) {
        return false;
    }

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);

        std::string_view status = doc["status"].get_string().value();
        if (status != "0000") {
            return false;
        }

        uint64_t ts_ms = 0;
        auto ts_field = doc["data"]["timestamp"];
        if (!ts_field.error()) {
            ts_ms = parse_timestamp_ms(ts_field.value());
        }
        if (ts_ms == 0) {
            ts_ms = steady_now_ms();
        }

        auto data = doc["data"].get_object();
        for (auto field : data) {
            std::string_view key = field.unescaped_key().value();
            if (key == "timestamp" || key == "payment_currency") continue;

            auto item = field.value().get_object();
            double bid = 0.0;
            double ask = 0.0;

            auto bids = item["bids"];
            if (!bids.error()) {
                for (auto b : bids.get_array()) {
                    std::string_view p = b["price"].get_string().value();
                    bid = kimp::opt::fast_stod(p);
                    break;
                }
            }
            auto asks = item["asks"];
            if (!asks.error()) {
                for (auto a : asks.get_array()) {
                    std::string_view p = a["price"].get_string().value();
                    ask = kimp::opt::fast_stod(p);
                    break;
                }
            }

            if (bid > 0.0 && ask > 0.0 && ask >= bid) {
                quotes[std::string(key)] = Quote{bid, ask, ts_ms};
            }
        }

        auto it = quotes.find("USDT");
        if (it != quotes.end()) {
            usdt_krw = (it->second.bid + it->second.ask) * 0.5;
        }
    } catch (const simdjson::simdjson_error&) {
        return false;
    }

    return !quotes.empty() && usdt_krw > 0.0;
}

bool fetch_bybit_all_tickers(kimp::exchange::RestClient& client,
                             std::unordered_map<std::string, Quote>& quotes) {
    quotes.clear();

    auto resp = client.get("/v5/market/tickers?category=linear");
    if (!resp.success) {
        return false;
    }

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);

        auto ret_code = doc["retCode"].get_int64().value();
        if (ret_code != 0) {
            return false;
        }

        uint64_t ts_ms = 0;
        auto time_field = doc["time"];
        if (!time_field.error()) {
            ts_ms = parse_timestamp_ms(time_field.value());
        }
        if (ts_ms == 0) {
            ts_ms = steady_now_ms();
        }

        auto list = doc["result"]["list"].get_array();
        for (auto item : list) {
            std::string_view symbol = item["symbol"].get_string().value();
            if (symbol.size() <= 4 || symbol.substr(symbol.size() - 4) != "USDT") continue;

            std::string base(symbol.substr(0, symbol.size() - 4));
            double bid = kimp::opt::fast_stod(item["bid1Price"].get_string().value());
            double ask = kimp::opt::fast_stod(item["ask1Price"].get_string().value());
            if (bid > 0.0 && ask > 0.0 && ask >= bid) {
                quotes[base] = Quote{bid, ask, ts_ms};
            }
        }
    } catch (const simdjson::simdjson_error&) {
        return false;
    }

    return !quotes.empty();
}

bool fetch_bithumb_symbol_orderbook(kimp::exchange::RestClient& client,
                                    const std::string& base,
                                    Quote& quote_out) {
    std::string target = "/public/orderbook/" + base + "_KRW?count=1";
    auto resp = client.get(target);
    if (!resp.success) return false;

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);

        std::string_view status = doc["status"].get_string().value();
        if (status != "0000") return false;

        auto data = doc["data"];
        uint64_t ts_ms = 0;
        auto ts = data["timestamp"];
        if (!ts.error()) {
            ts_ms = parse_timestamp_ms(ts.value());
        }
        if (ts_ms == 0) ts_ms = steady_now_ms();

        double bid = 0.0;
        double ask = 0.0;

        auto bids = data["bids"];
        if (!bids.error()) {
            for (auto b : bids.get_array()) {
                std::string_view p = b["price"].get_string().value();
                bid = kimp::opt::fast_stod(p);
                break;
            }
        }
        auto asks = data["asks"];
        if (!asks.error()) {
            for (auto a : asks.get_array()) {
                std::string_view p = a["price"].get_string().value();
                ask = kimp::opt::fast_stod(p);
                break;
            }
        }

        if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
        quote_out = Quote{bid, ask, ts_ms};
        return true;
    } catch (const simdjson::simdjson_error&) {
        return false;
    }
}

bool fetch_bybit_symbol_orderbook(kimp::exchange::RestClient& client,
                                  const std::string& base,
                                  Quote& quote_out) {
    std::string target = "/v5/market/orderbook?category=linear&symbol=" + base + "USDT&limit=1";
    auto resp = client.get(target);
    if (!resp.success) return false;

    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(resp.body);
        auto doc = parser.iterate(padded);

        int64_t ret_code = doc["retCode"].get_int64().value();
        if (ret_code != 0) return false;

        auto result = doc["result"];
        uint64_t ts_ms = 0;
        auto ts = result["ts"];
        if (!ts.error()) {
            ts_ms = parse_timestamp_ms(ts.value());
        }
        if (ts_ms == 0) ts_ms = steady_now_ms();

        double bid = 0.0;
        double ask = 0.0;

        auto bids = result["b"];
        if (!bids.error()) {
            for (auto row : bids.get_array()) {
                auto arr = row.get_array();
                for (auto cell : arr) {
                    std::string_view p = cell.get_string().value();
                    bid = kimp::opt::fast_stod(p);
                    break;
                }
                break;
            }
        }
        auto asks = result["a"];
        if (!asks.error()) {
            for (auto row : asks.get_array()) {
                auto arr = row.get_array();
                for (auto cell : arr) {
                    std::string_view p = cell.get_string().value();
                    ask = kimp::opt::fast_stod(p);
                    break;
                }
                break;
            }
        }

        if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
        quote_out = Quote{bid, ask, ts_ms};
        return true;
    } catch (const simdjson::simdjson_error&) {
        return false;
    }
}

void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "  --duration-sec <n>            Test duration in seconds (default: 600)\n"
        << "  --interval-ms <n>             Poll interval in ms (default: 1000)\n"
        << "  --entry-threshold <pct>       Entry threshold percent (default: -0.75)\n"
        << "  --max-verify-latency-ms <n>   Max verify latency (default: 800)\n"
        << "  --focus <CSV>                 Optional symbol filter (e.g., BTC,ETH,SOL)\n"
        << "  --config <path>               Config path for REST endpoints (default: config/config.yaml)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    int duration_sec = 600;
    int interval_ms = 1000;
    int max_verify_latency_ms = 800;
    double entry_threshold = kimp::TradingConfig::ENTRY_PREMIUM_THRESHOLD;
    std::string focus_csv;
    std::string config_path = "config/config.yaml";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration-sec" && i + 1 < argc) {
            duration_sec = std::stoi(argv[++i]);
        } else if (arg == "--interval-ms" && i + 1 < argc) {
            interval_ms = std::stoi(argv[++i]);
        } else if (arg == "--entry-threshold" && i + 1 < argc) {
            entry_threshold = std::stod(argv[++i]);
        } else if (arg == "--max-verify-latency-ms" && i + 1 < argc) {
            max_verify_latency_ms = std::stoi(argv[++i]);
        } else if (arg == "--focus" && i + 1 < argc) {
            focus_csv = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (duration_sec <= 0 || interval_ms <= 0 || max_verify_latency_ms <= 0) {
        std::cerr << "duration/interval/latency must be > 0\n";
        return 1;
    }

    auto focus_set = parse_focus_csv(focus_csv);

    kimp::RuntimeConfig config = kimp::ConfigLoader::load(config_path);
    std::string bithumb_rest = kimp::endpoints::BITHUMB_REST;
    std::string bybit_rest = kimp::endpoints::BYBIT_REST;

    auto bth_it = config.exchanges.find(kimp::Exchange::Bithumb);
    if (bth_it != config.exchanges.end() && !bth_it->second.rest_endpoint.empty()) {
        bithumb_rest = bth_it->second.rest_endpoint;
    }
    auto byb_it = config.exchanges.find(kimp::Exchange::Bybit);
    if (byb_it != config.exchanges.end() && !byb_it->second.rest_endpoint.empty()) {
        bybit_rest = byb_it->second.rest_endpoint;
    }

    net::io_context io_context;
    kimp::exchange::RestClient bithumb_client(io_context, extract_host(bithumb_rest));
    kimp::exchange::RestClient bybit_client(io_context, extract_host(bybit_rest));

    if (!bithumb_client.initialize() || !bybit_client.initialize()) {
        std::cerr << "Failed to initialize REST clients\n";
        return 1;
    }

    std::cout << "\n=== Gap Accuracy Diagnostic ===\n";
    std::cout << "Duration: " << duration_sec << "s | Interval: " << interval_ms
              << "ms | Entry threshold: " << entry_threshold
              << "% | Max verify latency: " << max_verify_latency_ms << "ms\n";
    if (!focus_set.empty()) {
        std::cout << "Focus symbols: " << focus_csv << "\n";
    }
    std::cout << "Bithumb host: " << extract_host(bithumb_rest)
              << " | Bybit host: " << extract_host(bybit_rest) << "\n";

    Stats stats;
    auto test_start = std::chrono::steady_clock::now();

    while (!g_stop.load(std::memory_order_acquire)) {
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - test_start).count();
        if (elapsed_sec >= duration_sec) break;

        auto round_start = std::chrono::steady_clock::now();
        ++stats.rounds;

        std::unordered_map<std::string, Quote> bth_quotes;
        std::unordered_map<std::string, Quote> byb_quotes;
        double usdt_krw = 0.0;

        auto bth_future = std::async(std::launch::async, [&]() {
            return fetch_bithumb_all_orderbook(bithumb_client, bth_quotes, usdt_krw);
        });
        auto byb_future = std::async(std::launch::async, [&]() {
            return fetch_bybit_all_tickers(bybit_client, byb_quotes);
        });

        bool bth_ok = bth_future.get();
        bool byb_ok = byb_future.get();
        if (!bth_ok || !byb_ok || usdt_krw <= 0.0) {
            ++stats.fetch_fail_rounds;
            std::cout << "[Round " << stats.rounds << "] fetch failed\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            continue;
        }

        std::vector<Candidate> all_rows;
        all_rows.reserve(512);

        for (const auto& [base, bth] : bth_quotes) {
            if (base == "USDT") continue;
            if (!focus_set.empty() && !focus_set.count(base)) continue;
            auto by_it = byb_quotes.find(base);
            if (by_it == byb_quotes.end()) continue;

            const auto& byb = by_it->second;
            Candidate c;
            c.base = base;
            c.kr_bid = bth.bid;
            c.kr_ask = bth.ask;
            c.by_bid = byb.bid;
            c.by_ask = byb.ask;
            c.initial_entry_pm = entry_premium(bth.ask, byb.bid, usdt_krw);
            c.exit_pm = exit_premium(bth.bid, byb.ask, usdt_krw);
            c.spread_pm = c.initial_entry_pm - c.exit_pm;
            all_rows.push_back(c);
        }

        std::sort(all_rows.begin(), all_rows.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.initial_entry_pm < b.initial_entry_pm;
                  });

        std::vector<Candidate> candidates;
        candidates.reserve(all_rows.size());
        for (const auto& row : all_rows) {
            if (row.initial_entry_pm <= entry_threshold) {
                candidates.push_back(row);
            }
        }

        stats.total_candidates += static_cast<int>(candidates.size());
        std::cout << "[Round " << std::setw(3) << stats.rounds
                  << "] usdt=" << std::fixed << std::setprecision(2) << usdt_krw
                  << " matched=" << all_rows.size()
                  << " candidates=" << candidates.size() << "\n";

        std::cout << "  "
                  << std::left << std::setw(10) << "SYMBOL"
                  << " " << std::right << std::setw(12) << "KR_bid"
                  << " " << std::setw(12) << "KR_ask"
                  << " " << std::setw(14) << "BY_bid"
                  << " " << std::setw(14) << "BY_ask"
                  << " " << std::setw(12) << "Entry%"
                  << " " << std::setw(12) << "Exit%"
                  << " " << std::setw(11) << "Spread%"
                  << " " << std::setw(8) << "SIG"
                  << "\n";

        for (const auto& row : all_rows) {
            const char* sig = row.initial_entry_pm <= entry_threshold ? "ENT" : "-";
            std::cout << "  "
                      << std::left << std::setw(10) << row.base
                      << " " << std::right << std::setw(12) << std::fixed << std::setprecision(2) << row.kr_bid
                      << " " << std::setw(12) << std::fixed << std::setprecision(2) << row.kr_ask
                      << " " << std::setw(14) << std::fixed << std::setprecision(6) << row.by_bid
                      << " " << std::setw(14) << std::fixed << std::setprecision(6) << row.by_ask
                      << " " << std::setw(12) << std::fixed << std::setprecision(4) << row.initial_entry_pm
                      << " " << std::setw(12) << std::fixed << std::setprecision(4) << row.exit_pm
                      << " " << std::setw(11) << std::fixed << std::setprecision(4) << row.spread_pm
                      << " " << std::setw(8) << sig
                      << "\n";
        }

        for (auto& c : candidates) {
            auto verify_start = std::chrono::steady_clock::now();
            Quote bth_verify{};
            Quote byb_verify{};

            auto vf1 = std::async(std::launch::async, [&]() {
                return fetch_bithumb_symbol_orderbook(bithumb_client, c.base, bth_verify);
            });
            auto vf2 = std::async(std::launch::async, [&]() {
                return fetch_bybit_symbol_orderbook(bybit_client, c.base, byb_verify);
            });

            bool ok1 = vf1.get();
            bool ok2 = vf2.get();
            c.verify_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - verify_start).count();

            if (!ok1 || !ok2) {
                ++stats.verify_failures;
                continue;
            }

            if (c.verify_latency_ms > max_verify_latency_ms) {
                c.skipped_latency = true;
                ++stats.skipped_due_latency;
                continue;
            }

            c.verify_entry_pm = entry_premium(bth_verify.ask, byb_verify.bid, usdt_krw);
            c.delta_pm = c.verify_entry_pm - c.initial_entry_pm;
            c.verified = true;
            ++stats.verified_candidates;

            double delta_abs = std::fabs(c.delta_pm);
            if (delta_abs > stats.worst_delta_abs) {
                stats.worst_delta_abs = delta_abs;
                stats.worst_delta_symbol = c.base;
            }

            if (c.verify_entry_pm > entry_threshold) {
                c.false_positive = true;
                ++stats.false_positives;
            }

            std::cout << "  - " << c.base
                      << " init=" << std::fixed << std::setprecision(4) << c.initial_entry_pm << "%"
                      << " verify=" << c.verify_entry_pm << "%"
                      << " delta=" << (c.verify_entry_pm - c.initial_entry_pm) << "%"
                      << " lag=" << c.verify_latency_ms << "ms";
            if (c.false_positive) {
                std::cout << "  <-- FALSE_POSITIVE";
            }
            std::cout << "\n";
        }

        auto round_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - round_start).count();
        int64_t sleep_ms = static_cast<int64_t>(interval_ms) - round_elapsed;
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }

    bithumb_client.shutdown();
    bybit_client.shutdown();

    std::cout << "\n=== Diagnostic Summary ===\n";
    std::cout << "Rounds: " << stats.rounds
              << " | Fetch-fail rounds: " << stats.fetch_fail_rounds << "\n";
    std::cout << "Candidates: " << stats.total_candidates
              << " | Verified: " << stats.verified_candidates
              << " | Verify-fail: " << stats.verify_failures
              << " | Skipped(latency): " << stats.skipped_due_latency << "\n";
    std::cout << "False positives: " << stats.false_positives << "\n";
    std::cout << "Worst |delta|: " << std::fixed << std::setprecision(4) << stats.worst_delta_abs
              << "% (" << (stats.worst_delta_symbol.empty() ? "-" : stats.worst_delta_symbol) << ")\n";

    if (stats.false_positives == 0) {
        std::cout << "RESULT: PASS (no false positives)\n";
        return 0;
    }

    std::cout << "RESULT: FAIL (false positives detected)\n";
    return 2;
}
