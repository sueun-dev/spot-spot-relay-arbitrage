#include "kimp/memory/atomic_bitset.hpp"
#include "kimp/strategy/entry_selection_bitmap.hpp"

#include <array>
#include <cassert>
#include <iostream>

namespace {

constexpr std::size_t kMaxSymbols = 16;

}  // namespace

int main() {
    using kimp::memory::AtomicBitset;
    using kimp::strategy::select_entry_candidates;

    std::cout << "=== Entry Selection Bitmap Regression Test ===\n";

    {
        AtomicBitset<kMaxSymbols> candidate_bits;
        AtomicBitset<kMaxSymbols> fired_bits;
        std::array<uint8_t, kMaxSymbols> position_state{};
        std::array<double, kMaxSymbols> premiums{};
        premiums[1] = -1.20;
        premiums[3] = -2.40;
        premiums[5] = -1.80;
        candidate_bits.set(1, true);
        candidate_bits.set(3, true);
        candidate_bits.set(5, true);

        const auto result = select_entry_candidates<kMaxSymbols>(
            8, 1, candidate_bits, fired_bits, position_state,
            [&](std::size_t idx) { return premiums[idx]; },
            [&](std::size_t idx) { return candidate_bits.test(idx); });

        assert(result.count == 1);
        assert(result.indices[0] == 3);
    }

    {
        AtomicBitset<kMaxSymbols> candidate_bits;
        AtomicBitset<kMaxSymbols> fired_bits;
        std::array<uint8_t, kMaxSymbols> position_state{};
        std::array<double, kMaxSymbols> premiums{};
        premiums[1] = -1.1;
        premiums[2] = -1.2;
        premiums[4] = -1.3;
        candidate_bits.set(1, true);
        candidate_bits.set(2, true);
        candidate_bits.set(4, true);
        fired_bits.set(7, true);

        const auto result = select_entry_candidates<kMaxSymbols>(
            8, 3, candidate_bits, fired_bits, position_state,
            [&](std::size_t idx) { return premiums[idx]; },
            [&](std::size_t idx) { return candidate_bits.test(idx); });

        assert(result.pending_new_signals == 1);
        assert(result.count == 2);
        assert(result.indices[0] == 1);
        assert(result.indices[1] == 2);
    }

    {
        AtomicBitset<kMaxSymbols> candidate_bits;
        AtomicBitset<kMaxSymbols> fired_bits;
        std::array<uint8_t, kMaxSymbols> position_state{};
        std::array<double, kMaxSymbols> premiums{};
        premiums[6] = -1.6;
        position_state[6] = 2;  // partial position

        const auto result = select_entry_candidates<kMaxSymbols>(
            8, 1, candidate_bits, fired_bits, position_state,
            [&](std::size_t idx) { return premiums[idx]; },
            [&](std::size_t idx) { return idx == 6; });

        assert(result.count == 1);
        assert(result.indices[0] == 6);
    }

    std::cout << "*** PASS: bitmap selector respects best, pending, partial rules ***\n";
    return 0;
}
