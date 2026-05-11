#include <iostream>
#include <cstring>
#include <cstdint>
#include "Page.h"
#include "SlottedPage.h"

int main()
{
    Page *page = new Page();
    SlottedPage *slotted_page = reinterpret_cast<SlottedPage *>(page->GetData());
    slotted_page->Init(100);

    std::cout << "Initial Free Space Pointer: " << slotted_page->GetFreeSpacePointer() << std::endl;

    const char *data1 = "Hello";
    const char *data2 = "World!!!";
    const char *data3 = "VariableLengthRecordDB";

    int slot1 = slotted_page->InsertRecord(data1, strlen(data1) + 1);
    std::cout << "Inserted Data 1 at slot: " << slot1 << ", Length: " << strlen(data1) + 1 << std::endl;

    int slot2 = slotted_page->InsertRecord(data2, strlen(data2) + 1);
    std::cout << "Inserted Data 2 at slot: " << slot2 << ", Length: " << strlen(data2) + 1 << std::endl;

    int slot3 = slotted_page->InsertRecord(data3, strlen(data3) + 1);
    std::cout << "Inserted Data 3 at slot: " << slot3 << ", Length: " << strlen(data3) + 1 << std::endl;

    std::cout << "Current Free Space Pointer: " << slotted_page->GetFreeSpacePointer() << std::endl;

    char buffer[256];
    uint16_t length;

    slotted_page->GetRecord(slot1, buffer, length);
    std::cout << "Read Slot 1: " << buffer << " (Length: " << length << ")" << std::endl;

    slotted_page->GetRecord(slot2, buffer, length);
    std::cout << "Read Slot 2: " << buffer << " (Length: " << length << ")" << std::endl;

    slotted_page->GetRecord(slot3, buffer, length);
    std::cout << "Read Slot 3: " << buffer << " (Length: " << length << ")" << std::endl;

    delete page;
    return 0;
}
