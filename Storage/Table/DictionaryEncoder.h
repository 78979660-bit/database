#pragma once

#include "../../Type/Value.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>

namespace Database
{
    /**
     * @brief Global Dictionary Encoder to transform Varchar strings into Integer IDs.
     * This achieves:
     * 1. Extreme memory compaction for repeated strings.
     * 2. O(1) comparison (==, !=) on SIMD/Registers directly instead of std::strcmp.
     * 3. Reduces bandwidth payload for CPU Caches on scans and joins.
     */
    class StringDictionaryEncoder
    {
    public:
        static StringDictionaryEncoder &GetInstance()
        {
            static StringDictionaryEncoder instance;
            return instance;
        }

        // Encode a raw string into a 32-bit Integer ID
        int32_t Encode(const std::string &str)
        {
            {
                std::shared_lock<std::shared_mutex> read_lock(mutex_);
                auto it = string_to_id_.find(str);
                if (it != string_to_id_.end())
                {
                    return it->second;
                }
            }

            std::unique_lock<std::shared_mutex> write_lock(mutex_);
            auto it = string_to_id_.find(str);
            if (it != string_to_id_.end())
            {
                return it->second;
            }

            int32_t new_id = static_cast<int32_t>(id_to_string_.size());
            string_to_id_[str] = new_id;
            id_to_string_.push_back(str);
            return new_id;
        }

        // Encode a Database::Value (VARCHAR) into an Integer Value
        Value EncodeValue(const Value &val)
        {
            if (val.GetTypeId() != TypeId::VARCHAR)
                return val;
            return Value(Encode(val.GetAsVarchar()));
        }

        // Decode a 32-bit ID back to its original string
        std::string Decode(int32_t id)
        {
            std::shared_lock<std::shared_mutex> read_lock(mutex_);
            if (id >= 0 && id < id_to_string_.size())
            {
                return id_to_string_[id];
            }
            return "";
        }

        // Decode back to a Database::Value
        Value DecodeValue(int32_t id)
        {
            return Value(Decode(id));
        }

    private:
        StringDictionaryEncoder() = default;

        std::unordered_map<std::string, int32_t> string_to_id_;
        std::vector<std::string> id_to_string_;
        std::shared_mutex mutex_; // Thread-safe concurrent Encoding/Decoding
    };

} // namespace Database