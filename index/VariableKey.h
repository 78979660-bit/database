#pragma once

#include <cstring>
#include <iostream>
#include <string>
#include <cstdint>

// A wrapper for variable length keys
// This is used for function arguments, NOT for storage layout
struct VariableKey
{
    char *data;
    uint32_t size;

    // Default constructor
    VariableKey() : data(nullptr), size(0) {}

    // Copy constructor (Deep Copy)
    VariableKey(const VariableKey &other)
    {
        size = other.size;
        if (size > 0)
        {
            data = new char[size];
            std::memcpy(data, other.data, size);
        }
        else
        {
            data = nullptr;
        }
    }

    // Assignment operator (Deep Copy)
    VariableKey &operator=(const VariableKey &other)
    {
        if (this == &other)
            return *this;

        if (data)
            delete[] data;

        size = other.size;
        if (size > 0)
        {
            data = new char[size];
            std::memcpy(data, other.data, size);
        }
        else
        {
            data = nullptr;
        }
        return *this;
    }

    // From string
    VariableKey(const std::string &s)
    {
        size = s.length() + 1; // Include null terminator
        data = new char[size];
        std::memcpy(data, s.c_str(), size);
    }

    // From C-string
    VariableKey(const char *s)
    {
        size = std::strlen(s) + 1; // Include null terminator
        data = new char[size];
        std::memcpy(data, s, size);
    }

    ~VariableKey()
    {
        if (data)
            delete[] data;
    }

    // Comparison for VariableKey
    bool operator<(const VariableKey &other) const
    {
        if (data == nullptr || other.data == nullptr)
            return false; // Or throw?
        return std::strcmp(data, other.data) < 0;
    }

    bool operator>(const VariableKey &other) const
    {
        if (data == nullptr || other.data == nullptr)
            return false;
        return std::strcmp(data, other.data) > 0;
    }

    bool operator==(const VariableKey &other) const
    {
        if (data == other.data)
            return true; // Handling both nullptr
        if (data == nullptr || other.data == nullptr)
            return false;
        return std::strcmp(data, other.data) == 0;
    }

    bool operator>=(const VariableKey &other) const
    {
        return !(*this < other);
    }

    friend std::ostream &operator<<(std::ostream &os, const VariableKey &key)
    {
        if (key.data)
            os << key.data;
        return os;
    }
};

struct VariableKeyComparator
{
    int operator()(const VariableKey &lhs, const VariableKey &rhs) const
    {
        if (lhs < rhs)
            return -1;
        if (lhs > rhs)
            return 1;
        return 0;
    }
};
