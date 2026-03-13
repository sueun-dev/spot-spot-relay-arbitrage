#pragma once

#include "kimp/core/config.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace kimp::strategy {

struct SpotRelayCandidate {
    std::string base;
    std::string bybit_symbol;
    double bithumb_bid_krw{0.0};
    double bithumb_ask_krw{0.0};
    double bithumb_bid_qty{0.0};
    double bithumb_ask_qty{0.0};
    double bybit_bid_usdt{0.0};
    double bybit_ask_usdt{0.0};
    double bybit_bid_qty{0.0};
    double bybit_ask_qty{0.0};
    double usdt_bid_krw{0.0};
    double usdt_ask_krw{0.0};
    double gross_edge_pct{0.0};          // Conservative: buy Bithumb ask, sell Bybit bid, convert USDT at Bithumb bid
    double net_edge_pct{0.0};
    double match_qty{0.0};
    double target_coin_qty{0.0};
    double max_tradable_usdt_at_best{0.0};
    double bithumb_top_krw{0.0};
    double bithumb_top_usdt{0.0};
    double bybit_top_usdt{0.0};
    double bybit_top_krw{0.0};
    double bithumb_total_fee_krw{0.0};
    double bybit_total_fee_usdt{0.0};
    double bybit_total_fee_krw{0.0};
    double total_fee_krw{0.0};
    double net_profit_krw{0.0};
    bool bithumb_can_fill_target{false};
    bool bybit_can_fill_target{false};
    bool both_can_fill_target{false};
    bool bithumb_withdraw_enabled{false};
    bool bybit_spot_trading{false};
    bool bybit_margin_enabled{false};
    std::string bybit_margin_mode;
    bool bybit_shortable{false};         // Auth-enriched borrow check result when available
    bool borrow_check_available{false};
    double bybit_max_short_qty{0.0};
    double bybit_max_short_usdt{0.0};
    bool bybit_deposit_enabled{false};
    bool bybit_withdraw_enabled{false};
    std::vector<std::string> shared_networks;

    bool enterable() const noexcept {
        return TradingConfig::entry_gate_passes(
            both_can_fill_target,
            match_qty,
            net_edge_pct,
            net_profit_krw);
    }

    bool transfer_ready() const noexcept {
        return bithumb_withdraw_enabled && bybit_deposit_enabled && !shared_networks.empty();
    }
};

class SpotRelayScanner {
public:
    struct Options {
        std::string json_output_path{"data/spot_relay_candidates.json"};
    };

    bool run(const RuntimeConfig& config, const Options& options, std::ostream& out);
};

} // namespace kimp::strategy
