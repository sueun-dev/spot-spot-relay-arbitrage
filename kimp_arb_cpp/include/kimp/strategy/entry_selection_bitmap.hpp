#pragma once

#include "kimp/memory/atomic_bitset.hpp"

#include <array>
#include <cstddef>
#include <algorithm>

namespace kimp::strategy {

template <std::size_t MaxSymbols>
struct EntrySelectionResult {
    std::array<std::size_t, MaxSymbols> indices{};
    std::size_t count{0};
    int active_positions{0};
    int pending_new_signals{0};
    int free_slots{0};

    void push(std::size_t idx) noexcept {
        if (count < MaxSymbols) {
            indices[count++] = idx;
        }
    }
};

template <std::size_t MaxSymbols, typename PremiumAccessor, typename QualifiedAccessor>
EntrySelectionResult<MaxSymbols> select_entry_candidates(
    std::size_t symbol_count,
    int max_positions,
    const memory::AtomicBitset<MaxSymbols>& candidate_bits,
    const memory::AtomicBitset<MaxSymbols>& signaled_bits,
    const std::array<uint8_t, MaxSymbols>& position_state,
    PremiumAccessor&& premium_at,
    QualifiedAccessor&& qualified_at) {

    EntrySelectionResult<MaxSymbols> result;

    for (std::size_t i = 0; i < symbol_count; ++i) {
        if (position_state[i] != 0) {
            ++result.active_positions;
        }
    }

    signaled_bits.for_each_set(symbol_count, [&](std::size_t idx) {
        if (position_state[idx] == 0) {
            ++result.pending_new_signals;
        }
    });

    const int used_slots = result.active_positions + result.pending_new_signals;
    result.free_slots = max_positions - used_slots;
    if (result.free_slots < 0) {
        result.free_slots = 0;
    }

    for (std::size_t idx = 0; idx < symbol_count; ++idx) {
        if (position_state[idx] == 2 && qualified_at(idx)) {
            result.push(idx);
        }
    }

    std::array<std::size_t, MaxSymbols> fresh_candidates;
    std::size_t fresh_count = 0;
    candidate_bits.for_each_set(symbol_count, [&](std::size_t idx) {
        const bool has_position = position_state[idx] != 0;
        const bool partial = position_state[idx] == 2;

        if (partial) {
            return;
        }
        if (has_position) {
            return;
        }
        if (result.free_slots <= 0) {
            return;
        }

        if (fresh_count < MaxSymbols) fresh_candidates[fresh_count++] = idx;
    });

    std::sort(fresh_candidates.begin(), fresh_candidates.begin() + fresh_count,
              [&](std::size_t lhs, std::size_t rhs) {
                  return premium_at(lhs) < premium_at(rhs);
              });

    for (std::size_t i = 0; i < fresh_count; ++i) {
        const std::size_t idx = fresh_candidates[i];
        if (result.free_slots <= 0) {
            break;
        }
        result.push(idx);
        --result.free_slots;
    }

    return result;
}

}  // namespace kimp::strategy
