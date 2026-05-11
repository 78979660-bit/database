#pragma once

#include <cstring>
#include <iostream>
#include <string>

/**
 * GenericKey is a template class that holds a fixed-size character array.
 * It strictly treats the data as binary/char data.
 * Size is the max size.
 */
template <size_t KeySize>
class GenericKey
{
public:
    char data[KeySize];

    GenericKey()
    {
        memset(data, 0, KeySize);
    }

    // Constructor from basic types for convenience
    GenericKey(int value)
    {
        memset(data, 0, KeySize);
        // This is a naive conversion, just for testing
        // In real strings, we'd use std::to_string
        std::string s = std::to_string(value);
        size_t len = s.length() < KeySize ? s.length() : KeySize - 1;
        memcpy(data, s.c_str(), len);
        data[len] = '\0';
    }

    GenericKey(const char *s)
    {
        SetString(s);
    }

    void SetString(const char *s)
    {
        memset(data, 0, KeySize);
        if (s != nullptr)
        {
            size_t len = strlen(s);
            if (len >= KeySize)
                len = KeySize - 1;
            memcpy(data, s, len);
        }
    }

    std::string ToString() const
    {
        // Ensure null termination for safety, though we maintain it
        return std::string(data);
    }

    // Comparison operators
    bool operator<(const GenericKey &other) const
    {
        return strcmp(data, other.data) < 0;
    }

    bool operator>(const GenericKey &other) const
    {
        return strcmp(data, other.data) > 0;
    }

    bool operator==(const GenericKey &other) const
    {
        return strcmp(data, other.data) == 0;
    }

    bool operator>=(const GenericKey &other) const
    {
        return strcmp(data, other.data) >= 0;
    }

    bool operator<=(const GenericKey &other) const
    {
        return strcmp(data, other.data) <= 0;
    }

    bool operator!=(const GenericKey &other) const
    {
        return strcmp(data, other.data) != 0;
    }

    friend std::ostream &operator<<(std::ostream &os, const GenericKey &key)
    {
        os << key.ToString();
        return os;
    }
};
