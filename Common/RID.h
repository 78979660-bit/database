#pragma once

#include <cstdint>
#include <string>

namespace Database
{

    using page_id_t = int32_t;
    using slot_id_t = uint16_t;
    using table_oid_t = uint32_t;

    class RID
    {
    public:
        RID() : page_id_(-1), slot_id_(0) {}
        RID(page_id_t page_id, slot_id_t slot_id) : page_id_(page_id), slot_id_(slot_id) {}

        page_id_t GetPageId() const { return page_id_; }
        slot_id_t GetSlotId() const { return slot_id_; }

        void Set(page_id_t page_id, slot_id_t slot_id)
        {
            page_id_ = page_id;
            slot_id_ = slot_id;
        }

        bool operator==(const RID &other) const
        {
            return page_id_ == other.page_id_ && slot_id_ == other.slot_id_;
        }

        std::string ToString() const
        {
            return "RID(" + std::to_string(page_id_) + ", " + std::to_string(slot_id_) + ")";
        }

    private:
        page_id_t page_id_;
        slot_id_t slot_id_;
    };

} // namespace Database

namespace std
{
    template <>
    struct hash<Database::RID>
    {
        size_t operator()(const Database::RID &rid) const
        {
            return hash<int32_t>()(rid.GetPageId()) ^ (hash<uint16_t>()(rid.GetSlotId()) << 1);
        }
    };
}