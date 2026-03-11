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
    double bybit_bid_usdt{0.0};
    double bybit_ask_usdt{0.0};
    double usdt_bid_krw{0.0};
    double usdt_ask_krw{0.0};
    double gross_edge_pct{0.0};          // Conservative: buy Bithumb ask, sell Bybit bid, convert USDT at Bithumb bid
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
