#pragma once

#include <vector>
#include <cstdint>

namespace Database
{

    /**
     * DeltaEncoding (差分编码) 存储相邻数值的差值。
     * 适用于有序或变化缓慢的数值列（如时间戳、自增ID），
     * 因为差值通常比原始值小，可以用更少的位来表示（或结合Bit-packing）。
     */
    class DeltaEncoder
    {
    public:
        struct DeltaResult
        {
            int32_t start_value;         // 序列起始值
            std::vector<int32_t> deltas; // 后续数值的差值
        };

        /**
         * 编码一组整数
         */
        static DeltaResult Compress(const std::vector<int32_t> &uncompressed_data)
        {
            DeltaResult result;
            if (uncompressed_data.empty())
            {
                result.start_value = 0;
                return result;
            }

            result.start_value = uncompressed_data[0];
            result.deltas.reserve(uncompressed_data.size() - 1);

            for (size_t i = 1; i < uncompressed_data.size(); ++i)
            {
                result.deltas.push_back(uncompressed_data[i] - uncompressed_data[i - 1]);
            }

            return result;
        }

        /**
         * 解码差分序列
         */
        static std::vector<int32_t> Decompress(const DeltaResult &compressed_result)
        {
            std::vector<int32_t> uncompressed_data;
            uncompressed_data.reserve(compressed_result.deltas.size() + 1);

            int32_t current_val = compressed_result.start_value;
            uncompressed_data.push_back(current_val);

            for (int32_t delta : compressed_result.deltas)
            {
                current_val += delta;
                uncompressed_data.push_back(current_val);
            }

            return uncompressed_data;
        }
    };

} // namespace Database
