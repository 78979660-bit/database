#pragma once

#include <string>
#include <vector>
#include <memory>
#include "../Type/Value.h"

namespace Database
{

    class Column
    {
    public:
        Column(std::string name, TypeId type)
            : name_(std::move(name)), type_(type) {}

        std::string GetName() const { return name_; }
        TypeId GetType() const { return type_; }

        // For fixed-length types, return size. For variable, return e.g. 0 or expected max size
        uint32_t GetFixedLength() const
        {
            if (type_ == TypeId::INTEGER)
                return sizeof(int32_t);
            return 0; // Variable or unknown
        }

    private:
        std::string name_;
        TypeId type_;
    };

    class Schema
    {
    public:
        Schema(std::vector<Column> columns) : columns_(std::move(columns)) {}

        const std::vector<Column> &GetColumns() const { return columns_; }

        const Column &GetColumn(uint32_t col_idx) const
        {
            return columns_[col_idx];
        }

        uint32_t GetColumnCount() const { return static_cast<uint32_t>(columns_.size()); }

        int GetColumnIndex(const std::string &name) const
        {
            for (uint32_t i = 0; i < columns_.size(); ++i)
            {
                if (columns_[i].GetName() == name)
                {
                    return i;
                }
            }
            return -1;
        }

    private:
        std::vector<Column> columns_;
    };

} // namespace Database