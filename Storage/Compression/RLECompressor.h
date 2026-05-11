#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>

namespace Database
{

    /**
     * Run-Length Encoding (RLE) compresses sequential identical values.
     * Useful for low-cardinality sorted columns in analytical workloads.
     */
    class RLECompressor
    {
    public:
        struct RLEBatch
        {
            int32_t value;
            uint32_t count;
        };

        /**
         * Compress an array of integer values into RLE format.
         */
        static std::vector<RLEBatch> Compress(const std::vector<int32_t> &uncompressed_data)
        {
            std::vector<RLEBatch> compressed_data;
            if (uncompressed_data.empty())
            {
                return compressed_data;
            }

            int32_t current_val = uncompressed_data[0];
            uint32_t current_count = 1;

            for (size_t i = 1; i < uncompressed_data.size(); ++i)
            {
                if (uncompressed_data[i] == current_val)
                {
                    current_count++;
                }
                else
                {
                    compressed_data.push_back({current_val, current_count});
                    current_val = uncompressed_data[i];
                    current_count = 1;
                }
            }
            compressed_data.push_back({current_val, current_count});

            return compressed_data;
        }

        /**
         * Decompress RLE format back to a sequence of integers.
         */
        static std::vector<int32_t> Decompress(const std::vector<RLEBatch> &compressed_data)
        {
            std::vector<int32_t> uncompressed_data;
            for (const auto &batch : compressed_data)
            {
                for (uint32_t i = 0; i < batch.count; ++i)
                {
                    uncompressed_data.push_back(batch.value);
                }
            }
            return uncompressed_data;
        }

        // In a sophisticated engine, we would read directly from RLEBatch via an iterator
        // to evaluate predicates without decompressing.
    };

} // namespace Database