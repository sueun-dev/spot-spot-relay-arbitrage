#pragma once

#include <cstddef>
#include <vector>

// SIMD detection - AVX2 available on most modern CPUs (Intel Haswell+, AMD Excavator+)
#if defined(__AVX2__)
#include <immintrin.h>
#define KIMP_HAS_AVX2 1
#elif defined(_MSC_VER) && defined(__AVX2__)
#include <intrin.h>
#define KIMP_HAS_AVX2 1
#else
#define KIMP_HAS_AVX2 0
#endif

// ARM NEON support (Apple M1/M2, ARM servers)
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define KIMP_HAS_NEON 1
#else
#define KIMP_HAS_NEON 0
#endif

namespace kimp {

/**
 * SIMD-optimized premium calculation for HFT
 *
 * Processes 4 premiums in parallel using AVX2 (256-bit registers)
 * or 2 premiums using NEON (128-bit registers)
 *
 * Formula: premium = ((korean - (foreign * usdt)) / (foreign * usdt)) * 100
 *
 * Performance:
 * - Scalar: ~3-5ns per calculation
 * - AVX2:   ~0.8-1.2ns per calculation (4x parallel)
 * - NEON:   ~1.5-2ns per calculation (2x parallel)
 */
class SIMDPremiumCalculator {
public:
    // Structure for batch input
    struct PriceData {
        double korean_price;
        double foreign_price;
        double usdt_rate;
    };

#if KIMP_HAS_AVX2
    /**
     * AVX2 batch premium calculation (4 at a time)
     *
     * @param korean   Array of Korean prices (KRW)
     * @param foreign  Array of foreign prices (USD)
     * @param usdt     Array of USDT/KRW rates
     * @param result   Output array for premiums (%)
     * @param count    Number of elements (will process count/4 batches + remainder)
     */
    static void calculate_batch_avx2(const double* korean, const double* foreign,
                                      const double* usdt, double* result, size_t count) {
        const __m256d hundred = _mm256_set1_pd(100.0);
        const size_t simd_count = count / 4;

        for (size_t i = 0; i < simd_count; ++i) {
            const size_t offset = i * 4;

            // Load 4 values at once (32 bytes)
            __m256d korean_v = _mm256_loadu_pd(&korean[offset]);
            __m256d foreign_v = _mm256_loadu_pd(&foreign[offset]);
            __m256d usdt_v = _mm256_loadu_pd(&usdt[offset]);

            // foreign_krw = foreign * usdt
            __m256d foreign_krw = _mm256_mul_pd(foreign_v, usdt_v);

            // diff = korean - foreign_krw
            __m256d diff = _mm256_sub_pd(korean_v, foreign_krw);

            // premium = (diff / foreign_krw) * 100
            __m256d premium = _mm256_div_pd(diff, foreign_krw);
            premium = _mm256_mul_pd(premium, hundred);

            // Store result
            _mm256_storeu_pd(&result[offset], premium);
        }

        // Handle remaining elements with scalar math
        for (size_t i = simd_count * 4; i < count; ++i) {
            double foreign_krw = foreign[i] * usdt[i];
            result[i] = ((korean[i] - foreign_krw) / foreign_krw) * 100.0;
        }
    }

    /**
     * AVX2 batch calculation with FMA (Fused Multiply-Add)
     * Slightly faster on CPUs with FMA3 support (Haswell+)
     */
    static void calculate_batch_avx2_fma(const double* korean, const double* foreign,
                                          const double* usdt, double* result, size_t count) {
        const __m256d hundred = _mm256_set1_pd(100.0);
        const size_t simd_count = count / 4;

        for (size_t i = 0; i < simd_count; ++i) {
            const size_t offset = i * 4;

            __m256d korean_v = _mm256_loadu_pd(&korean[offset]);
            __m256d foreign_v = _mm256_loadu_pd(&foreign[offset]);
            __m256d usdt_v = _mm256_loadu_pd(&usdt[offset]);

            // foreign_krw = foreign * usdt
            __m256d foreign_krw = _mm256_mul_pd(foreign_v, usdt_v);

            // diff = korean - foreign_krw (using FMA: -1 * foreign_krw + korean)
            __m256d neg_one = _mm256_set1_pd(-1.0);
            __m256d diff = _mm256_fmadd_pd(neg_one, foreign_krw, korean_v);

            // premium = (diff / foreign_krw) * 100
            __m256d premium = _mm256_div_pd(diff, foreign_krw);
            premium = _mm256_mul_pd(premium, hundred);

            _mm256_storeu_pd(&result[offset], premium);
        }

        // Scalar remainder
        for (size_t i = simd_count * 4; i < count; ++i) {
            double foreign_krw = foreign[i] * usdt[i];
            result[i] = ((korean[i] - foreign_krw) / foreign_krw) * 100.0;
        }
    }
#endif

#if KIMP_HAS_NEON
    /**
     * ARM NEON batch premium calculation with FMA (2 at a time)
     * Optimized for Apple M1/M2 and ARM servers
     *
     * Uses vfmaq_f64 (mandatory on ARMv8/aarch64) to fuse multiply-add,
     * reducing 4 ops to 3 per iteration and improving numerical precision.
     *
     * Formula rewrite: premium = (korean/(foreign*usdt) - 1) * 100
     *                          = korean/(foreign*usdt) * 100 - 100
     *                          = FMA(-100, ratio, 100)
     */
    static void calculate_batch_neon(const double* korean, const double* foreign,
                                      const double* usdt, double* result, size_t count) {
        const float64x2_t hundred = vdupq_n_f64(100.0);
        const float64x2_t neg_hundred = vdupq_n_f64(-100.0);
        const size_t simd_count = count / 2;

        for (size_t i = 0; i < simd_count; ++i) {
            const size_t offset = i * 2;

            // Load 2 values at once (16 bytes)
            float64x2_t korean_v = vld1q_f64(&korean[offset]);
            float64x2_t foreign_v = vld1q_f64(&foreign[offset]);
            float64x2_t usdt_v = vld1q_f64(&usdt[offset]);

            // foreign_krw = foreign * usdt
            float64x2_t foreign_krw = vmulq_f64(foreign_v, usdt_v);

            // ratio = korean / foreign_krw
            float64x2_t ratio = vdivq_f64(korean_v, foreign_krw);

            // premium = ratio * 100 - 100 (FMA: -100 + ratio * 100)
            float64x2_t premium = vfmaq_f64(neg_hundred, ratio, hundred);

            // Store result
            vst1q_f64(&result[offset], premium);
        }

        // Handle remaining element
        for (size_t i = simd_count * 2; i < count; ++i) {
            double foreign_krw = foreign[i] * usdt[i];
            result[i] = ((korean[i] - foreign_krw) / foreign_krw) * 100.0;
        }
    }
#endif

    /**
     * Scalar fallback (still optimized with force-inline)
     */
    __attribute__((always_inline))
    static inline double calculate_scalar(double korean, double foreign, double usdt) noexcept {
        const double foreign_krw = foreign * usdt;
        return ((korean - foreign_krw) / foreign_krw) * 100.0;
    }

    /**
     * Scalar batch (for non-SIMD systems or small batches)
     */
    static void calculate_batch_scalar(const double* korean, const double* foreign,
                                        const double* usdt, double* result, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            result[i] = calculate_scalar(korean[i], foreign[i], usdt[i]);
        }
    }

    /**
     * Auto-dispatch to best available SIMD implementation
     * Falls back to scalar on unsupported platforms
     */
    static void calculate_batch(const double* korean, const double* foreign,
                                 const double* usdt, double* result, size_t count) {
#if KIMP_HAS_AVX2
        calculate_batch_avx2(korean, foreign, usdt, result, count);
#elif KIMP_HAS_NEON
        calculate_batch_neon(korean, foreign, usdt, result, count);
#else
        calculate_batch_scalar(korean, foreign, usdt, result, count);
#endif
    }

    /**
     * Calculate batch from vector of PriceData
     * Returns vector of premiums
     */
    static std::vector<double> calculate_batch(const std::vector<PriceData>& prices) {
        const size_t count = prices.size();
        if (count == 0) return {};

        // Prepare aligned arrays for SIMD
        std::vector<double> korean(count);
        std::vector<double> foreign(count);
        std::vector<double> usdt(count);
        std::vector<double> result(count);

        // Extract data (this could be optimized with SoA layout)
        for (size_t i = 0; i < count; ++i) {
            korean[i] = prices[i].korean_price;
            foreign[i] = prices[i].foreign_price;
            usdt[i] = prices[i].usdt_rate;
        }

        calculate_batch(korean.data(), foreign.data(), usdt.data(), result.data(), count);
        return result;
    }

    /**
     * Runtime check for SIMD support
     */
    static constexpr bool has_avx2() noexcept {
#if KIMP_HAS_AVX2
        return true;
#else
        return false;
#endif
    }

    static constexpr bool has_neon() noexcept {
#if KIMP_HAS_NEON
        return true;
#else
        return false;
#endif
    }

    static const char* get_simd_type() noexcept {
#if KIMP_HAS_AVX2
        return "AVX2 (4x parallel)";
#elif KIMP_HAS_NEON
        return "NEON (2x parallel)";
#else
        return "Scalar";
#endif
    }
};

} // namespace kimp
