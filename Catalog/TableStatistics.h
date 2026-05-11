#pragma once

#include <vector>
#include <map>
#include <string>
#include <memory>
#include <cmath>
#include <functional>
#include <numeric>
#include <algorithm>
#include "../Type/Value.h"

namespace Database
{

    class Histogram
    {
    public:
        virtual ~Histogram() = default;
        // Basic selectivity estimation for an equality predicate: col = val
        virtual double GetEqualitySelectivity(const Value &val) const = 0;
        // Basic selectivity estimation for a range predicate: col < val
        virtual double GetLessSelectivity(const Value &val) const = 0;
        virtual double GetGreaterSelectivity(const Value &val) const = 0;
    };

    /**
     * @brief EquiDepthHistogram splits data into buckets of approximately equal number of tuples.
     * Useful for skewed data distribution.
     */
    class EquiDepthHistogram : public Histogram
    {
    public:
        struct Bucket
        {
            Value min_val;
            Value max_val;
            size_t count;
            size_t distinct_count;
        };

        EquiDepthHistogram(std::vector<Bucket> buckets, size_t total_count)
            : buckets_(std::move(buckets)), total_count_(total_count) {}

        double GetEqualitySelectivity(const Value &val) const override
        {
            if (total_count_ == 0 || buckets_.empty())
                return 0.0;
            for (const auto &b : buckets_)
            {
                // If val falls in this bucket
                if (!(val < b.min_val) && !(b.max_val < val))
                {
                    // Assuming uniform distribution inside the bucket
                    double bucket_sel = static_cast<double>(b.count) / total_count_;
                    return (b.distinct_count > 0) ? bucket_sel / b.distinct_count : bucket_sel;
                }
            }
            return 0.0; // Out of range
        }

        double GetLessSelectivity(const Value &val) const override
        {
            if (total_count_ == 0 || buckets_.empty())
                return 0.0;
            double selectivity = 0.0;
            for (const auto &b : buckets_)
            {
                if (b.max_val < val)
                {
                    selectivity += static_cast<double>(b.count) / total_count_;
                }
                else if (b.min_val < val && !(b.max_val < val))
                {
                    // Fractional overlap (simplified continuous assumption)
                    // (val - min) / (max - min) would be used for numeric types
                    // As a fallback for generic Value, assume 50% overlap if it hits inside.
                    double bucket_sel = static_cast<double>(b.count) / total_count_;
                    selectivity += bucket_sel * 0.5;
                    break;
                }
                else
                {
                    break;
                }
            }
            return selectivity;
        }

        double GetGreaterSelectivity(const Value &val) const override
        {
            return std::max(0.0, 1.0 - GetLessSelectivity(val) - GetEqualitySelectivity(val));
        }

    private:
        std::vector<Bucket> buckets_;
        size_t total_count_;
    };

    /**
     * @brief HyperLogLog accurately estimates Number of Distinct Values (NDV)
     * using minimal memory footprint (e.g. 64-256 registers).
     */
    class HyperLogLog
    {
    public:
        HyperLogLog(size_t b = 6) : b_(b), m_(1 << b), registers_(m_, 0) {}

        void Add(const Value &val)
        {
            // Generate a 64-bit hash
            uint64_t hash_val = std::hash<std::string>{}(val.GetAsVarchar());

            // Extract index from first b bits
            uint32_t index = hash_val >> (64 - b_);

            // Extract remainder
            uint64_t w = hash_val << b_;

            // Count leading zeros of the remainder
            uint8_t rho = 1;
            if (w == 0)
            {
                rho = 64 - b_ + 1;
            }
            else
            {
#if defined(__GNUC__) || defined(__clang__)
                rho = __builtin_clzll(w) + 1;
#else
                uint64_t temp = w;
                int count = 0;
                while ((temp & 0x8000000000000000ULL) == 0 && count < 64)
                {
                    temp <<= 1;
                    count++;
                }
                rho = count + 1;
#endif
            }

            registers_[index] = std::max(registers_[index], rho);
        }

        size_t GetEstimate() const
        {
            double Z = 0.0;
            for (uint8_t val : registers_)
            {
                Z += 1.0 / (1ULL << val);
            }
            double E = alpha_m() * m_ * m_ / Z;

            // Apply small range correction
            if (E <= 2.5 * m_)
            {
                size_t V = std::count(registers_.begin(), registers_.end(), 0);
                if (V > 0)
                {
                    E = m_ * std::log(static_cast<double>(m_) / V);
                }
            }
            return static_cast<size_t>(E);
        }

    private:
        double alpha_m() const
        {
            switch (m_)
            {
            case 16:
                return 0.673;
            case 32:
                return 0.697;
            case 64:
                return 0.709;
            default:
                return 0.7213 / (1.0 + 1.079 / m_);
            }
        }

        size_t b_;
        size_t m_;
        std::vector<uint8_t> registers_;
    };
    struct ColumnStats
    {
        double min_value_{0};
        double max_value_{0};
        size_t null_count_{0};
        size_t distinct_count_{0};
        std::shared_ptr<Histogram> histogram_{nullptr};
    };
    struct TableStatistics
    {
        size_t tuple_count_{0};
        size_t page_count_{0};
        std::map<std::string, ColumnStats> column_stats_;

        TableStatistics() = default;
        TableStatistics(size_t tuples, size_t pages) : tuple_count_(tuples), page_count_(pages) {}
    };

} // namespace Database