#pragma once

#include <memory>
#include <algorithm>
#include <cmath>
#include "../Catalog/TableMetadata.h"
#include "LogicalPlanNode.h"

namespace Database
{

    class CostModel
    {
    public:
        static constexpr double CPU_WEIGHT = 0.2;
        static constexpr double IO_WEIGHT = 1.0;
        static constexpr double NETWORK_WEIGHT = 1.5;
        static constexpr double MEMORY_WEIGHT = 0.05;
        static constexpr double INDEX_HEIGHT_ESTIMATE = 3.0;

        static constexpr size_t L3_CACHE_SIZE = 8 * 1024 * 1024;

        CostModel() = default;
        ~CostModel() = default;

        double EstimateSeqScanCost(const TableMetadata *table) const
        {
            if (!table) return 0.0;
            return (table->stats_.page_count_ * IO_WEIGHT) + (table->stats_.tuple_count_ * CPU_WEIGHT);
        }

        double EstimateIndexScanCost(const TableMetadata *table, double selectivity = 0.01) const
        {
            if (!table) return 0.0;
            double tree_traversal_cost = INDEX_HEIGHT_ESTIMATE * IO_WEIGHT;
            double row_count = table->stats_.tuple_count_ * selectivity;
            double row_lookup_io_cost = std::max(1.0, row_count) * IO_WEIGHT;
            double row_lookup_cpu_cost = std::max(1.0, row_count) * CPU_WEIGHT;
            return tree_traversal_cost + row_lookup_io_cost + row_lookup_cpu_cost;
        }

        double EstimateNestedLoopJoinCost(size_t left_rows, size_t right_rows) const {
            double cpu_cost = (left_rows * right_rows) * CPU_WEIGHT;
            double io_cost = (left_rows * right_rows * 0.1) * IO_WEIGHT;
            return cpu_cost + io_cost;
        }

        double EstimateSortMergeJoinCost(size_t left_rows, size_t right_rows, bool left_sorted = false, bool right_sorted = false) const {
            double left_sort_cost = left_sorted ? 0 : (left_rows * std::log2(std::max<size_t>(2, left_rows)) * CPU_WEIGHT);
            double right_sort_cost = right_sorted ? 0 : (right_rows * std::log2(std::max<size_t>(2, right_rows)) * CPU_WEIGHT);
            double merge_cost = (left_rows + right_rows) * CPU_WEIGHT;
            return left_sort_cost + right_sort_cost + merge_cost;
        }

        double EstimateHashJoinCost(size_t left_rows, size_t right_rows, size_t right_row_size = 64) const {
            double build_cost = right_rows * CPU_WEIGHT;
            double probe_cost = left_rows * CPU_WEIGHT;
            size_t build_size = right_rows * right_row_size;
            double cache_miss_penalty = (build_size > L3_CACHE_SIZE) ? (probe_cost * 1.5) : 0;
            return build_cost + probe_cost + cache_miss_penalty;
        }

        double EstimateRadixHashJoinCost(size_t left_rows, size_t right_rows, size_t right_row_size = 64) const {
            double partition_cost = (left_rows + right_rows) * CPU_WEIGHT * 0.5;
            double build_cost = right_rows * CPU_WEIGHT;
            double probe_cost = left_rows * CPU_WEIGHT;
            return partition_cost + build_cost + probe_cost;
        }
    };

} // namespace Database
