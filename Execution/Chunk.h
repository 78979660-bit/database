#pragma once

#include <vector>
#include <memory>
#include <cstdint>
#include <cassert>

#include "Vector.h"

namespace Database
{

    // Define a standard chunk/batch size for the vectorized execution
    constexpr size_t STANDARD_VECTOR_SIZE = 4096;

    /**
     * @brief A Chunk (or RecordBatch) represents a set of column vectors housing a batch of rows.
     * The volcano iterator's Next() method will return exactly one Chunk per invocation.
     */
    class Chunk
    {
    public:
        Chunk() : count_(0), capacity_(0), sel_vector_(nullptr) {}

        explicit Chunk(size_t capacity)
            : count_(0), capacity_(capacity), sel_vector_(nullptr), rids_(capacity) {}

        // Add a column vector to the chunk
        void AddVector(std::shared_ptr<Vector> vector)
        {
            vectors_.push_back(vector);
        }

        inline size_t GetColumnCount() const { return vectors_.size(); }
        inline size_t GetCapacity() const { return capacity_; }

        inline size_t GetCount() const { return count_; }
        inline void SetCount(size_t count)
        {
            assert(count <= capacity_);
            count_ = count;
        }

        // Access specific column vector
        std::shared_ptr<Vector> GetVector(size_t col_idx) const
        {
            assert(col_idx < vectors_.size());
            return vectors_[col_idx];
        }

        // Selection vector support inside the chunk
        inline bool HasSelectionVector() const { return sel_vector_ != nullptr; }
        inline std::shared_ptr<SelectionVector> GetSelectionVector() const { return sel_vector_; }
        inline void SetSelectionVector(std::shared_ptr<SelectionVector> sel_vector) { sel_vector_ = sel_vector; }

        // Fallback for getting an individual Tuple logic (if interacting with legacy code)
        // Note: For pure vectorization, avoid calling this in a tight loop!
        std::vector<Value> GetRow(size_t physical_index) const
        {
            size_t actual_index = HasSelectionVector() ? sel_vector_->GetIndex(physical_index) : physical_index;
            std::vector<Value> values;
            values.reserve(GetColumnCount());
            for (const auto &vec : vectors_)
            {
                values.push_back(vec->GetValue(actual_index));
            }
            return values;
        }

        void Reset()
        {
            count_ = 0;
            sel_vector_ = nullptr;
            if (rids_.size() != capacity_)
            {
                rids_.resize(capacity_);
            }
        }

        // Optional RID storage for data modification executors
        inline void SetRID(size_t index, RID rid) { rids_[index] = rid; }
        inline RID GetRID(size_t index) const { return rids_[index]; }

    private:
        std::vector<std::shared_ptr<Vector>> vectors_;
        size_t count_;
        size_t capacity_;
        std::shared_ptr<SelectionVector> sel_vector_;
        std::vector<RID> rids_;
    };

} // namespace Database
