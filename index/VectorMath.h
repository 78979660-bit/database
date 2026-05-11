#pragma once
#include <vector>
#include <immintrin.h> // For AVX / AVX2

namespace Database
{
    namespace VectorMath
    {

        // 使用 AVX2 加速的内积计算 (可以用于余弦相似度)
        inline float DotProductSIMD(const float *a, const float *b, size_t dim)
        {
            float result = 0.0f;
            size_t i = 0;

            // 初始化一个累加器全0的 256-bit (8-float) 向量
            __m256 sum256 = _mm256_setzero_ps();
            for (; i + 7 < dim; i += 8)
            {
                // 每次读取 8 个 float
                __m256 va = _mm256_loadu_ps(a + i);
                __m256 vb = _mm256_loadu_ps(b + i);
                // 执行 FMA 计算 sum256 += va * vb
                sum256 = _mm256_fmadd_ps(va, vb, sum256);
            }

            // 横向累加 256 位寄存器中的 8 个 float
            alignas(32) float tmp[8];
            _mm256_store_ps(tmp, sum256);
            for (int j = 0; j < 8; ++j)
                result += tmp[j];

            // 处理剩余的维度
            for (; i < dim; ++i)
            {
                result += a[i] * b[i];
            }

            return result;
        }

    } // namespace VectorMath
} // namespace Database
