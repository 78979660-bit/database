#pragma once

#include <vector>
#include <cstring>
#include "../../Type/Value.h"
#include "../../Catalog/Schema.h"
#include "../../Common/RID.h"
#include "../../Concurrency/Transaction.h"

namespace Database
{

    // 用于 MVCC 可见性判断的元信息
    struct TupleMeta
    {
        txn_id_t insert_txn_id_{INVALID_TXN_ID}; // 哪一个事务创建了这个版本
        txn_id_t delete_txn_id_{INVALID_TXN_ID}; // 哪一个事务删除了这个版本 (逻辑删除)
        bool is_deleted_{false};                 // 是否被彻底标记为删除
    };

    class Tuple
{
public:
    Tuple() : rid_(), data_ptr_(nullptr), size_(0), allocated_(false) {}

    // Construct from values based on schema
    Tuple(const std::vector<Value> &values, const Schema *schema)
    {
        allocated_ = true;
        uint32_t size = 0;
        for (const auto &val : values)
        {
            size += val.GetSerializedSize();
        }

        data_.resize(size);
        data_ptr_ = data_.data();
        size_ = size;
        
        char *ptr = data_.data();
        for (const auto &val : values)
        {
            val.SerializeTo(ptr);
            ptr += val.GetSerializedSize();
        }
    }

    // Construct from raw data WITHOUT ALLOCATION (Zero-Copy View)
    Tuple(const char *data, uint32_t size, RID rid) : rid_(rid), data_ptr_(data), size_(size), allocated_(false)
    {
    }

    // Copy constructor
    Tuple(const Tuple &other) : rid_(other.rid_), meta_(other.meta_), size_(other.size_), allocated_(other.allocated_) 
    {
        if (allocated_) {
            data_ = other.data_;
            data_ptr_ = data_.data();
        } else {
            data_ptr_ = other.data_ptr_;
        }
    }

    Tuple(Tuple &&other) noexcept
        : data_(std::move(other.data_)),
          data_ptr_(nullptr),
          size_(other.size_),
          allocated_(other.allocated_),
          rid_(other.rid_),
          meta_(other.meta_)
    {
        data_ptr_ = allocated_ ? data_.data() : other.data_ptr_;
        other.data_ptr_ = nullptr;
        other.size_ = 0;
        other.allocated_ = false;
    }
    
    // Assignment operator
    Tuple& operator=(const Tuple &other) {
        if (this == &other) return *this;
        rid_ = other.rid_;
        meta_ = other.meta_;
        size_ = other.size_;
        allocated_ = other.allocated_;
        if (allocated_) {
            data_ = other.data_;
            data_ptr_ = data_.data();
        } else {
            data_ptr_ = other.data_ptr_;
            data_ = std::vector<char>(); // Force destruction instead of clear()
        }
        return *this;
    }

    Tuple& operator=(Tuple &&other) noexcept {
        if (this == &other) return *this;
        data_ = std::move(other.data_);
        size_ = other.size_;
        allocated_ = other.allocated_;
        rid_ = other.rid_;
        meta_ = other.meta_;
        data_ptr_ = allocated_ ? data_.data() : other.data_ptr_;
        other.data_ptr_ = nullptr;
        other.size_ = 0;
        other.allocated_ = false;
        return *this;
    }

    Value GetValue(const Schema *schema, uint32_t col_idx) const
    {
        const char *ptr = data_ptr_;
        for (uint32_t i = 0; i < col_idx; ++i)
        {
            TypeId type = schema->GetColumn(i).GetType();
            Value temp = Value::DeserializeFrom(ptr, type);
            ptr += temp.GetSerializedSize();
        }

        TypeId target_type = schema->GetColumn(col_idx).GetType();
        return Value::DeserializeFrom(ptr, target_type);
    }

    const char *GetData() const { return data_ptr_; }
    uint32_t GetLength() const { return size_; }

    RID GetRID() const { return rid_; }
    void SetRID(RID rid) { rid_ = rid; }

    TupleMeta GetMeta() const { return meta_; }
    void SetMeta(const TupleMeta &meta) { meta_ = meta; }

private:
    std::vector<char> data_; // Only used if allocated_ is true
    const char *data_ptr_;
    uint32_t size_;
    bool allocated_;

    RID rid_;
    TupleMeta meta_; 
};

} // namespace Database
