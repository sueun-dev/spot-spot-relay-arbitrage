#include "kimp/memory/atomic_bitset.hpp"

#include <array>
#include <cassert>
#include <iostream>

int main() {
    using kimp::memory::AtomicBitset;

    std::cout << "=== Atomic Bitset Regression Test ===\n";

    AtomicBitset<130> bits;
    bits.set(0, true);
    bits.set(64, true);
    bits.set(129, true);

    assert(bits.test(0));
    assert(bits.test(64));
    assert(bits.test(129));
    assert(bits.count() == 3);

    std::array<std::size_t, 3> seen{};
    std::size_t count = 0;
    bits.for_each_set(130, [&](std::size_t idx) {
        seen[count++] = idx;
    });

    assert(count == 3);
    assert(seen[0] == 0);
    assert(seen[1] == 64);
    assert(seen[2] == 129);

    bits.set(64, false);
    assert(!bits.test(64));
    assert(bits.count() == 2);

    bits.clear_all();
    assert(bits.count() == 0);

    std::cout << "*** PASS: set/clear/count/iteration all stable ***\n";
    return 0;
}
