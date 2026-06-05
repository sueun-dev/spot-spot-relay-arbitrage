#include "kimp/memory/atomic_bitset.hpp"

#include <array>
#include "test_check.hpp"
#include <iostream>

int main() {
    using kimp::memory::AtomicBitset;

    std::cout << "=== Atomic Bitset Regression Test ===\n";

    AtomicBitset<130> bits;
    bits.set(0, true);
    bits.set(64, true);
    bits.set(129, true);

    KIMP_CHECK(bits.test(0));
    KIMP_CHECK(bits.test(64));
    KIMP_CHECK(bits.test(129));
    KIMP_CHECK(bits.count() == 3);

    std::array<std::size_t, 3> seen{};
    std::size_t count = 0;
    bits.for_each_set(130, [&](std::size_t idx) {
        seen[count++] = idx;
    });

    KIMP_CHECK(count == 3);
    KIMP_CHECK(seen[0] == 0);
    KIMP_CHECK(seen[1] == 64);
    KIMP_CHECK(seen[2] == 129);

    bits.set(64, false);
    KIMP_CHECK(!bits.test(64));
    KIMP_CHECK(bits.count() == 2);

    bits.clear_all();
    KIMP_CHECK(bits.count() == 0);

    std::cout << "*** PASS: set/clear/count/iteration all stable ***\n";
    return 0;
}
