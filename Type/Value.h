#ifndef DATABASE_VALUE_H
#define DATABASE_VALUE_H

#include <iostream>
#include <string>
#include <variant>
#include <vector>
#include <cstring>
#include <cassert>
#include <cstdint>

namespace Database
{

    enum class TypeId
    {
        INVALID = 0,
        BOOLEAN,
        TINYINT,
        SMALLINT,
        INTEGER,
        BIGINT,
        DECIMAL,
        VARCHAR,
        TIMESTAMP,
    };

    class Value
    {
    public:
        Value() : type_(TypeId::INVALID) {}
        Value(TypeId type) : type_(type) {}

        // Integer constructor
        Value(int32_t i) : type_(TypeId::INTEGER), integer_(i) {}

        // Varchar constructor
        Value(const std::string &s) : type_(TypeId::VARCHAR), varchar_(s) {}

        TypeId GetTypeId() const { return type_; }
        bool IsNull() const { return type_ == TypeId::INVALID; }

        template <typename T>
        T GetAs() const;

        int32_t GetAsInteger() const
        {
            if (type_ == TypeId::INTEGER)
                return integer_;
            // In real system, throw exception or convert
            return 0;
        }

        std::string GetAsVarchar() const
        {
            if (type_ == TypeId::VARCHAR)
                return varchar_;
            if (type_ == TypeId::INTEGER)
                return std::to_string(integer_);
            return "";
        }

        // Serialize to char buffer
        void SerializeTo(char *dest) const
        {
            if (type_ == TypeId::INTEGER)
            {
                std::memcpy(dest, &integer_, sizeof(int32_t));
            }
            else if (type_ == TypeId::VARCHAR)
            {
                uint32_t len = varchar_.length();
                std::memcpy(dest, &len, sizeof(uint32_t));
                std::memcpy(dest + sizeof(uint32_t), varchar_.c_str(), len);
            }
        }

        // Deserialize from char buffer
        static Value DeserializeFrom(const char *src, TypeId type)
        {
            if (type == TypeId::INTEGER)
            {
                int32_t val;
                std::memcpy(&val, src, sizeof(int32_t));
                return Value(val);
            }
            else if (type == TypeId::VARCHAR)
            {
                uint32_t len;
                std::memcpy(&len, src, sizeof(uint32_t));
                std::string s(src + sizeof(uint32_t), len);
                return Value(s);
            }
            return Value(TypeId::INVALID);
        }

        // Get serialized size
        uint32_t GetSerializedSize() const
        {
            if (type_ == TypeId::INTEGER)
            {
                return sizeof(int32_t);
            }
            else if (type_ == TypeId::VARCHAR)
            {
                return sizeof(uint32_t) + varchar_.length();
            }
            return 0;
        }

        // Comparison for sorting/filtering
        bool operator==(const Value &other) const
        {
            if (type_ != other.type_)
                return false;
            if (type_ == TypeId::INTEGER)
                return integer_ == other.integer_;
            if (type_ == TypeId::VARCHAR)
                return varchar_ == other.varchar_;
            return false;
        }

        bool operator<(const Value &other) const
        {
            if (type_ != other.type_)
                return false; // Should handle type mismatch
            if (type_ == TypeId::INTEGER)
                return integer_ < other.integer_;
            if (type_ == TypeId::VARCHAR)
                return varchar_ < other.varchar_;
            return false;
        }

    private:
        TypeId type_;
        int32_t integer_ = 0;
        std::string varchar_;
    };

    template <>
    inline int32_t Value::GetAs<int32_t>() const
    {
        return GetAsInteger();
    }

    template <>
    inline std::string Value::GetAs<std::string>() const
    {
        return GetAsVarchar();
    }

} // namespace Database

#endif // DATABASE_VALUE_H