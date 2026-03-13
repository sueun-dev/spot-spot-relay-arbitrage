#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace kimp::memory {

template <std::size_t BitCount>
class AtomicBitset {
public:
    static constexpr std::size_t WORD_BITS = 64;
    static constexpr std::size_t WORD_COUNT = (BitCount + WORD_BITS - 1) / WORD_BITS;

    AtomicBitset() = default;
    AtomicBitset(const AtomicBitset&) = delete;
    AtomicBitset& operator=(const AtomicBitset&) = delete;

    void set(std::size_t index, bool enabled) noexcept {
        if (index >= BitCount) {
            return;
        }

        const std::size_t word_idx = index / WORD_BITS;
        const uint64_t mask = uint64_t{1} << (index % WORD_BITS);
        // Check-before-RMW: skip atomic RMW if bit already in desired state
        uint64_t current = words_[word_idx].load(std::memory_order_relaxed);
        if (enabled) {
            if (current & mask) return;  // Already set
            words_[word_idx].fetch_or(mask, std::memory_order_release);
        } else {
            if (!(current & mask)) return;  // Already clear
            words_[word_idx].fetch_and(~mask, std::memory_order_release);
        }
    }

    bool test(std::size_t index) const noexcept {
        if (index >= BitCount) {
            return false;
        }

        const std::size_t word_idx = index / WORD_BITS;
        const uint64_t mask = uint64_t{1} << (index % WORD_BITS);
        return (words_[word_idx].load(std::memory_order_acquire) & mask) != 0;
    }

    void clear_all() noexcept {
        for (auto& word : words_) {
            word.store(0, std::memory_order_release);
        }
    }

    std::size_t count(std::size_t limit = BitCount) const noexcept {
        if (limit > BitCount) {
            limit = BitCount;
        }

        std::size_t total = 0;
        const std::size_t word_limit = (limit + WORD_BITS - 1) / WORD_BITS;
        for (std::size_t word_idx = 0; word_idx < word_limit; ++word_idx) {
            uint64_t word = words_[word_idx].load(std::memory_order_acquire);
            if (word_idx + 1 == word_limit && (limit % WORD_BITS) != 0) {
                word &= ((uint64_t{1} << (limit % WORD_BITS)) - 1);
            }
            total += static_cast<std::size_t>(std::popcount(word));
        }
        return total;
    }

    template <typename Fn>
    void for_each_set(std::size_t limit, Fn&& fn) const {
        if (limit > BitCount) {
            limit = BitCount;
        }

        const std::size_t word_limit = (limit + WORD_BITS - 1) / WORD_BITS;
        for (std::size_t word_idx = 0; word_idx < word_limit; ++word_idx) {
            uint64_t word = words_[word_idx].load(std::memory_order_acquire);
            if (word_idx + 1 == word_limit && (limit % WORD_BITS) != 0) {
                word &= ((uint64_t{1} << (limit % WORD_BITS)) - 1);
            }

            while (word != 0) {
                const unsigned bit = std::countr_zero(word);
                const std::size_t index = word_idx * WORD_BITS + bit;
                fn(index);
                word &= (word - 1);
            }
        }
    }

private:
    std::array<std::atomic<uint64_t>, WORD_COUNT> words_{};
};

}  // namespace kimp::memory
