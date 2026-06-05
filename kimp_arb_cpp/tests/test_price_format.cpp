#include "kimp/core/price_format.hpp"

#include "test_check.hpp"
#include <iostream>

int main() {
    using kimp::format::format_decimal_trimmed;

    KIMP_CHECK(format_decimal_trimmed(104489000.0) == "104489000");
    KIMP_CHECK(format_decimal_trimmed(1461.0) == "1461");
    KIMP_CHECK(format_decimal_trimmed(0.0051) == "0.0051");
    KIMP_CHECK(format_decimal_trimmed(0.01000000) == "0.01");
    KIMP_CHECK(format_decimal_trimmed(-0.0) == "0");
    KIMP_CHECK(format_decimal_trimmed(123.456789123, 8) == "123.45678912");

    std::cout << "*** PASS: price formatting preserves low-priced KRW ticks without fake rounding ***\n";
    return 0;
}
