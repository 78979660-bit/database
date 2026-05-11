#include <iostream>
#include <vector>
#include <cassert>
#include <memory>

#include "../Storage/PAXPage.h"
#include "../Catalog/Schema.h"
#include "../Type/Value.h"

using namespace Database;

// Helper to assert conditions
#define ASSERT_TRUE(cond)                                                                                 \
    if (!(cond))                                                                                          \
    {                                                                                                     \
        std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        exit(1);                                                                                          \
    }

void test_pax_page_init()
{
    std::cout << "Testing PAXPage Initialization..." << std::endl;

    // Create a schema
    std::vector<Column> cols;
    cols.emplace_back("id", TypeId::INTEGER);
    cols.emplace_back("age", TypeId::INTEGER);
    Schema schema(cols);

    // Initialize PAX Page
    PAXPage page;
    page.Init(10, schema);

    // GetPageId() belongs to base class and might not be set by Init,
    // but SetPageId writes to PAGE_ID_OFFSET natively.
    ASSERT_TRUE(page.GetColumnCount() == 2);
    ASSERT_TRUE(page.GetNumTuples() == 0);

    // Check Meta offsets are calculated
    PAXPage::ColumnMeta meta0 = page.GetColumnMeta(0);
    PAXPage::ColumnMeta meta1 = page.GetColumnMeta(1);

    ASSERT_TRUE(meta0.length > 0);
    ASSERT_TRUE(meta1.length > 0);
    ASSERT_TRUE(meta0.offset < meta1.offset);

    std::cout << "PAXPage Init test passed.\n";
}

int main()
{
    std::cout << "Starting PAXPage Tests...\n";
    test_pax_page_init();
    std::cout << "All PAXPage Tests Passed!\n";
    return 0;
}