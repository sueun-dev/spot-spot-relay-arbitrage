#pragma once

#include "kimp/memory/atomic_bitset.hpp"

#include <array>
#include <cstddef>

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

    if (max_positions <= 1) {
        if (result.free_slots == 0) {
            for (std::size_t idx = 0; idx < symbol_count; ++idx) {
                if (position_state[idx] == 2 && qualified_at(idx)) {
                    result.push(idx);
                    break;
                }
            }
            return result;
        }

        double best_premium = 100.0;
        std::size_t best_idx = MaxSymbols;
        candidate_bits.for_each_set(symbol_count, [&](std::size_t idx) {
            if (position_state[idx] == 1) {
                return;
            }
            const double premium = premium_at(idx);
            if (premium < best_premium) {
                best_premium = premium;
                best_idx = idx;
            }
        });

        if (best_idx != MaxSymbols) {
            result.push(best_idx);
        }
        return result;
    }

    for (std::size_t idx = 0; idx < symbol_count; ++idx) {
        if (position_state[idx] == 2 && qualified_at(idx)) {
            result.push(idx);
        }
    }

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

        result.push(idx);
        if (result.free_slots > 0) {
            --result.free_slots;
        }
    });

    return result;
}

}  // namespace kimp::strategy
