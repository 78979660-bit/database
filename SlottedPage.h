#pragma once

#include <cstring>
#include <iostream>
#include <cstdint>
#include "Page.h"

// Slotted Page Layout
// ------------------------------------------------------------------
// | Header (PageId, LSN, SlotCount, FreeSpacePointer, ...) |
// ------------------------------------------------------------------
// | Slot[0] (Offset, Length) | Slot[1] | ... | Slot[N] |
// ------------------------------------------------------------------
// | ... (Free Space) ... |
// ------------------------------------------------------------------
// | ... | Data[1] | Data[0] |
// ------------------------------------------------------------------

struct Slot
{
    uint16_t offset;
    uint16_t length;
};

class SlottedPage
{
public:
    page_id_t page_id_;
    lsn_t lsn_;
    int slot_count_;
    int free_space_pointer_;
    page_id_t next_page_id_;
    int zone_min_;
    int zone_max_;
    int bloom_filter_;
    int padding_;
    Slot slots_[0];

    char *GetData() { return reinterpret_cast<char *>(this); }
    const char *GetData() const { return reinterpret_cast<const char *>(this); }

    void Init(page_id_t page_id)
    {
        page_id_ = page_id;
        lsn_ = 0;
        slot_count_ = 0;
        free_space_pointer_ = PAGE_SIZE; // Points to the end of the page
        next_page_id_ = INVALID_PAGE_ID;
    }

    // Header Getters/Setters
    int GetSlotCount() const { return slot_count_; }
    void SetSlotCount(int count) { slot_count_ = count; }

    int GetFreeSpacePointer() const { return free_space_pointer_; }
    void SetFreeSpacePointer(int ptr) { free_space_pointer_ = ptr; }

    // Slot Access
    Slot GetSlot(int index) const
    {
        if (index < 0 || index >= slot_count_)
            return {0, 0};
        return slots_[index];
    }

    void SetSlot(int index, uint16_t offset, uint16_t length)
    {
        slots_[index].offset = offset;
        slots_[index].length = length;
    }

    // Insert record
    int InsertRecord(const char *data, uint16_t length)
    {
        int free_space = free_space_pointer_;
        int slot_count = slot_count_;
        // Space needed for new slot
        int header_end = 36 + (slot_count + 1) * sizeof(Slot); 

        // Check if we have enough space
        if (header_end + length > free_space)
        {
            return -1; // Not enough space
        }

        // 1. Allocate space at end of free space block
        int new_data_offset = free_space - length;
        free_space_pointer_ = new_data_offset;

        // 2. Copy data
        memcpy(GetData() + new_data_offset, data, length);

        // 3. Add new slot
        slots_[slot_count].offset = new_data_offset;
        slots_[slot_count].length = length;
        slot_count_ = slot_count + 1;

        return slot_count;
    }

    // Delete record 
    void DeleteRecord(int index)
    {
        if (index < 0 || index >= slot_count_)
            return;
        slots_[index].length = 0;
    }

    // Read record
    void GetRecord(int index, char *buffer, uint16_t &length)
    {
        if (index < 0 || index >= slot_count_) {
            length = 0;
            return;
        }
        Slot s = slots_[index];
        if (s.length == 0)
        {
            length = 0;
            return;
        }
        length = s.length;
        memcpy(buffer, GetData() + s.offset, s.length);
    }

    // Pointer to record data
    const char *GetRecordData(int index)
    {
        if (index < 0 || index >= slot_count_) return nullptr;
        Slot s = slots_[index];
        if (s.length == 0)
            return nullptr;
        return GetData() + s.offset;
    }

    // Next Page Id Accessor
    page_id_t GetNextPageId() const { return next_page_id_; }
    void SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }
};


