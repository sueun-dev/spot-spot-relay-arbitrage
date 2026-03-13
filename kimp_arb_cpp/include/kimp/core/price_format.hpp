#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <fmt/format.h>

namespace kimp::format {

inline std::string format_decimal_trimmed(double value, int max_decimals = 8) {
    if (!std::isfinite(value)) {
        return "0";
    }

    const int decimals = std::max(0, max_decimals);
    std::string out = fmt::format("{:.{}f}", value, decimals);

    if (decimals > 0) {
        while (!out.empty() && out.back() == '0') {
            out.pop_back();
        }
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }

    if (out.empty() || out == "-0") {
        return "0";
    }
    return out;
}

}  // namespace kimp::format
