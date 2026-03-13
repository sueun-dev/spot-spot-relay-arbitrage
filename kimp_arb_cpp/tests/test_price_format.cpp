#include "kimp/core/price_format.hpp"

#include <cassert>
#include <iostream>

int main() {
    using kimp::format::format_decimal_trimmed;

    assert(format_decimal_trimmed(104489000.0) == "104489000");
    assert(format_decimal_trimmed(1461.0) == "1461");
    assert(format_decimal_trimmed(0.0051) == "0.0051");
    assert(format_decimal_trimmed(0.01000000) == "0.01");
    assert(format_decimal_trimmed(-0.0) == "0");
    assert(format_decimal_trimmed(123.456789123, 8) == "123.45678912");

    std::cout << "*** PASS: price formatting preserves low-priced KRW ticks without fake rounding ***\n";
    return 0;
}
