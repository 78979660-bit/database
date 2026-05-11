#pragma once

#include <vector>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include <string>

#include "../Type/Value.h"

namespace Database
{

    /**
     * @brief SelectionVector is used to mask out rows that are filtered out or deleted
     * without having to physically move data around in the Vector.
     */
    class SelectionVector
    {
    public:
        explicit SelectionVector(size_t capacity) : sel_vector_(capacity) {}

        inline void SetIndex(size_t pos, size_t index) { sel_vector_[pos] = index; }
        inline size_t GetIndex(size_t pos) const { return sel_vector_[pos]; }
        inline size_t *Data() { return sel_vector_.data(); }

    private:
        std::vector<size_t> sel_vector_;
    };

    /**
     * @brief Base class for vectorized column data.
     * To achieve high performance, ideally we use templatized subclasses (FlatVector<T>)
     * to avoid virtual calls in tight loops.
     */
    class Vector
    {
    public:
        Vector(TypeId type, size_t capacity) : type_(type), capacity_(capacity), null_bitmap_(capacity, 0) {}
        virtual ~Vector() = default;

        TypeId GetType() const { return type_; }
        size_t GetCapacity() const { return capacity_; }

        // Null checks
        inline bool IsNull(size_t index) const { return null_bitmap_[index]; }
        inline void SetNull(size_t index, bool is_null) { null_bitmap_[index] = is_null ? 1 : 0; }

        /**
         * @brief Fallback for row-based or polymorphic extraction.
         * In a fully vectorized engine, direct array access via downcasting is preferred.
         */
        virtual Value GetValue(size_t index) const = 0;
        virtual void SetValue(size_t index, const Value &val) = 0;

    protected:
        TypeId type_;
        size_t capacity_;
        // [拆毁防线: L3 Cache 优化] 用 uint8_t 替代 std::vector<bool> 防止内部位运算代理对象破坏自动向量化和预取
        std::vector<uint8_t> null_bitmap_;
    };

    /**
     * @brief FlatVector implements a contiguous array for a specific physical type.
     */
    template <typename T>
    class FlatVector : public Vector
    {
    public:
        FlatVector(TypeId type, size_t capacity) : Vector(type, capacity), data_(capacity) {}

        inline const T *Data() const { return data_.data(); }
        inline T *Data() { return data_.data(); }

        Value GetValue(size_t index) const override;
        void SetValue(size_t index, const Value &val) override;

    private:
        std::vector<T> data_;
    };

    // Specializations for GetValue / SetValue for int32_t
    template <>
    inline Value FlatVector<int32_t>::GetValue(size_t index) const
    {
        if (IsNull(index))
            return Value(type_); // return null equivalent
        return Value(data_[index]);
    }

    template <>
    inline void FlatVector<int32_t>::SetValue(size_t index, const Value &val)
    {
        if (val.GetTypeId() == TypeId::INVALID)
        { // Assuming INVALID means null
            SetNull(index, true);
        }
        else
        {
            SetNull(index, false);
            data_[index] = val.GetAsInteger();
        }
    }

    // Specializations for GetValue / SetValue for std::string (varchar)
    template <>
    inline Value FlatVector<std::string>::GetValue(size_t index) const
    {
        if (IsNull(index))
            return Value(type_);
        return Value(data_[index]);
    }

    template <>
    inline void FlatVector<std::string>::SetValue(size_t index, const Value &val)
    {
        if (val.GetTypeId() == TypeId::INVALID)
        {
            SetNull(index, true);
        }
        else
        {
            SetNull(index, false);
            data_[index] = val.GetAsVarchar();
        }
    }

    // Specializations for GetValue / SetValue for Value (Generic)
    template <>
    inline Value FlatVector<Value>::GetValue(size_t index) const
    {
        if (IsNull(index))
            return Value(TypeId::INVALID);
        return data_[index];
    }

    template <>
    inline void FlatVector<Value>::SetValue(size_t index, const Value &val)
    {
        if (val.GetTypeId() == TypeId::INVALID)
        {
            SetNull(index, true);
        }
        else
        {
            SetNull(index, false);
            data_[index] = val;
        }
    }

    /**
     * @brief DictionaryVector implements a dictionary-compressed string/value vector.
     * Prevents duplicate data allocation by holding indices into a unique dictionary.
     */
    template <typename T>
    class DictionaryVector : public Vector
    {
    public:
        DictionaryVector(TypeId type, size_t capacity, std::shared_ptr<Vector> dict, std::shared_ptr<Vector> ids)
            : Vector(type, capacity), dictionary_(dict), indices_(ids) {}

        Value GetValue(size_t index) const override;
        void SetValue(size_t index, const Value &val) override
        {
            throw std::runtime_error("SetValue not supported on compressed DictionaryVector");
        }

        inline std::shared_ptr<Vector> GetDictionary() const { return dictionary_; }
        inline std::shared_ptr<Vector> GetIndices() const { return indices_; }

    private:
        std::shared_ptr<Vector> dictionary_; // holds distinct FlatVector<T>
        std::shared_ptr<Vector> indices_;    // holds FlatVector<uint16_t>
    };

    // Dictionary template specializations
    template <>
    inline Value DictionaryVector<std::string>::GetValue(size_t index) const
    {
        if (IsNull(index))
            return Value(type_);
        auto dict_index_value = indices_->GetValue(index);
        size_t dict_idx = static_cast<size_t>(dict_index_value.GetAsInteger());
        return dictionary_->GetValue(dict_idx);
    }

    template <>
    inline Value DictionaryVector<int32_t>::GetValue(size_t index) const
    {
        if (IsNull(index))
            return Value(type_);
        auto dict_index_value = indices_->GetValue(index);
        size_t dict_idx = static_cast<size_t>(dict_index_value.GetAsInteger());
        return dictionary_->GetValue(dict_idx);
    }

    /**
     * @brief RLEVector implements a run-length encoded vector.
     * Useful for consecutive repeating values in columnar databases to save I/O and RAM.
     */
    template <typename T>
    class RLEVector : public Vector
    {
    public:
        RLEVector(TypeId type, size_t capacity, std::shared_ptr<Vector> values, std::shared_ptr<Vector> runs)
            : Vector(type, capacity), values_(values), runs_(runs) {}

        Value GetValue(size_t index) const override
        {
            // Evaluates dynamically. A more optimized execution engine directly processes runs.
            size_t current_idx = 0;
            for (size_t i = 0; i < runs_->GetCapacity(); ++i)
            {
                int run_len = runs_->GetValue(i).GetAsInteger();
                current_idx += run_len;
                if (index < current_idx)
                {
                    return values_->GetValue(i);
                }
            }
            return Value(type_); // default return
        }

        void SetValue(size_t index, const Value &val) override
        {
            throw std::runtime_error("SetValue not supported on compressed RLEVector");
        }

        inline std::shared_ptr<Vector> GetValues() const { return values_; }
        inline std::shared_ptr<Vector> GetRuns() const { return runs_; }

    private:
        std::shared_ptr<Vector> values_; // FlatVector<T> holding values
        std::shared_ptr<Vector> runs_;   // FlatVector<int32_t> holding lengths
    };

} // namespace Database
