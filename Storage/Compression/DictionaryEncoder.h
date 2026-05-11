#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>

namespace Database
{

    /**
     * DictionaryEncoding maps variable-length string values or categorical values
     * into compact integer IDs.
     * Highly effective for columns with mostly repetitive strings (e.g. Country="USA").
     */
    class DictionaryEncoder
    {
    public:
        struct EncodingResult
        {
            std::vector<std::string> dictionary; // Dict mapping ID -> String
            std::vector<uint32_t> data;          // Data mapped to ID
        };

        /**
         * Compress an array of strings into Dictionary Encoded mapping.
         */
        static EncodingResult Compress(const std::vector<std::string> &uncompressed_data)
        {
            EncodingResult result;
            std::unordered_map<std::string, uint32_t> string_to_id;
            uint32_t next_id = 0;

            for (const auto &str_val : uncompressed_data)
            {
                auto it = string_to_id.find(str_val);
                if (it == string_to_id.end())
                {
                    string_to_id[str_val] = next_id;
                    result.dictionary.push_back(str_val);
                    result.data.push_back(next_id);
                    next_id++;
                }
                else
                {
                    result.data.push_back(it->second);
                }
            }

            return result;
        }

        /**
         * Decompress mapping back to string vector.
         */
        static std::vector<std::string> Decompress(const EncodingResult &compressed_result)
        {
            std::vector<std::string> uncompressed_data;
            uncompressed_data.reserve(compressed_result.data.size());

            for (uint32_t id : compressed_result.data)
            {
                // Safety check skipped for performance in actual code
                uncompressed_data.push_back(compressed_result.dictionary[id]);
            }

            return uncompressed_data;
        }
    };

} // namespace Database