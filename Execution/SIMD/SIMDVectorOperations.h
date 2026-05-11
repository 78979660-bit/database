#pragma once

#include <cstdint>
#include <cstddef>
#include <immintrin.h> // For AVX2 / AVX-512

namespace Database
{
    namespace SIMD
    {
        /**
         * @brief Perform SIMD accelerated element-wise comparison between two integer arrays.
         * Useful for vectorizing filter / logical expressions.
         * @param left Array 1 (e.g. column data)
         * @param right Array 2 (e.g. column data or broadcasted constant)
         * @param result Result array (1 for true, 0 for false)
         * @param count Number of elements
         */
        inline void CompareEqualInt32(const int32_t *left, const int32_t *right, int32_t *result, size_t count)
        {
            size_t i = 0;

#ifdef __AVX512F__
            // AVX-512 processes 16 integers at a time (512 bits)
            for (; i + 15 < count; i += 16)
            {
                __m512i v_left = _mm512_loadu_si512((const __m512i *)(left + i));
                __m512i v_right = _mm512_loadu_si512((const __m512i *)(right + i));
                __mmask16 mask = _mm512_cmpeq_epi32_mask(v_left, v_right);

                // Convert mask back to 32-bit integers mapping to 1 or 0
                // For a highly optimized engine we'd keep results as a bitmap / selection vector directly!
                for (int j = 0; j < 16; ++j)
                {
                    result[i + j] = (mask & (1 << j)) ? 1 : 0;
                }
            }
#elif defined(__AVX2__)
            // AVX2 processes 8 integers at a time (256 bits)
            __m256i v_one = _mm256_set1_epi32(1);
            for (; i + 7 < count; i += 8)
            {
                __m256i v_left = _mm256_loadu_si256((const __m256i *)(left + i));
                __m256i v_right = _mm256_loadu_si256((const __m256i *)(right + i));
                // Compare -> sets bits to all 1's if equal, 0 if not
                __m256i v_cmp = _mm256_cmpeq_epi32(v_left, v_right);

                // Mask with v_one to get 1 or 0 instead of -1 or 0
                __m256i v_res = _mm256_and_si256(v_cmp, v_one);

                _mm256_storeu_si256((__m256i *)(result + i), v_res);
            }
#endif
            // Scalar fallback for remaining elements (or if no AVX)
            for (; i < count; ++i)
            {
                result[i] = (left[i] == right[i]) ? 1 : 0;
            }
        }

        inline void CompareLessThanInt32(const int32_t *left, const int32_t *right, int32_t *result, size_t count)
        {
            size_t i = 0;

#ifdef __AVX512F__
            for (; i + 15 < count; i += 16)
            {
                __m512i v_left = _mm512_loadu_si512((const __m512i *)(left + i));
                __m512i v_right = _mm512_loadu_si512((const __m512i *)(right + i));
                __mmask16 mask = _mm512_cmplt_epi32_mask(v_left, v_right);
                for (int j = 0; j < 16; ++j)
                {
                    result[i + j] = (mask & (1 << j)) ? 1 : 0;
                }
            }
#elif defined(__AVX2__)
            __m256i v_one = _mm256_set1_epi32(1);
            for (; i + 7 < count; i += 8)
            {
                __m256i v_left = _mm256_loadu_si256((const __m256i *)(left + i));
                __m256i v_right = _mm256_loadu_si256((const __m256i *)(right + i));

                // Note: AVX2 uses _mm256_cmpgt_epi32. a < b ==> b > a
                __m256i v_cmp = _mm256_cmpgt_epi32(v_right, v_left);
                __m256i v_res = _mm256_and_si256(v_cmp, v_one);

                _mm256_storeu_si256((__m256i *)(result + i), v_res);
            }
#endif
            for (; i < count; ++i)
            {
                result[i] = (left[i] < right[i]) ? 1 : 0;
            }
        }

        // Additional operators (greater than, sum/aggregations) could be extended similarly...
        inline int32_t SumInt32(const int32_t *data, size_t count)
        {
            int32_t sum = 0;
            size_t i = 0;

#ifdef __AVX2__
            __m256i v_sum = _mm256_setzero_si256();
            for (; i + 7 < count; i += 8)
            {
                __m256i v_data = _mm256_loadu_si256((const __m256i *)(data + i));
                v_sum = _mm256_add_epi32(v_sum, v_data);
            }
            // Horizontal addition of 8 integers
            int32_t partial_sums[8];
            _mm256_storeu_si256((__m256i *)partial_sums, v_sum);
            for (int j = 0; j < 8; ++j)
                sum += partial_sums[j];
#endif
            for (; i < count; ++i)
            {
                sum += data[i];
            }
            return sum;
        }

    } // namespace SIMD
} // namespace Database