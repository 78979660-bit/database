#include <iostream>
#include <vector>
#include <cassert>

#include "../Type/Value.h"

using namespace Database;

// Helper to assert conditions
#define ASSERT_TRUE(cond)                                                                                 \
    if (!(cond))                                                                                          \
    {                                                                                                     \
        std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        exit(1);                                                                                          \
    }

void test_value_creation()
{
    std::cout << "Testing Types and Values..." << std::endl;

    // Integer handling
    Value v1(42);
    ASSERT_TRUE(v1.GetTypeId() == TypeId::INTEGER);
    ASSERT_TRUE(v1.GetAsInteger() == 42);

    Value v2(100);
    ASSERT_TRUE(v1 < v2);
    ASSERT_TRUE(!(v2 < v1));

    // Varchar handling
    Value s1(std::string("hello"));
    ASSERT_TRUE(s1.GetTypeId() == TypeId::VARCHAR);
    ASSERT_TRUE(s1.GetAsVarchar() == "hello");

    std::cout << "Type and Value basic test passed.\n";
}

int main()
{
    std::cout << "Starting Type Tests...\n";
    test_value_creation();
    std::cout << "All Type Tests Passed!\n";
    return 0;
}