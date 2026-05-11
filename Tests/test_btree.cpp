#include <iostream>
#include <string>
#include <vector>
#include "DiskManager.h"
#include "BufferPoolManager.h"
#include "Index/BPlusTree.h"
#include "Index/GenericKey.h"

// Define the key type
using MyKey = GenericKey<32>; // 32 bytes key
using MyValue = int;
// Comparator is just less than wrapper, but our operators are already defined
struct MyComparator
{
    int operator()(const MyKey &lhs, const MyKey &rhs) const
    {
        if (lhs < rhs)
            return -1;
        if (lhs > rhs)
            return 1;
        return 0;
    }
};

int main()
{
    // Basic setup
    auto *disk = new DiskManager("btree_test.db");
    auto *bpm = new BufferPoolManager(50, disk); // 50 pages buffer

    // B+ Tree with GenericKey<32>, int value, MyComparator
    BPlusTree<MyKey, MyValue, MyComparator> tree("index1", bpm);

    // 1. Insert some string keys
    int n = 20;
    std::cout << "Insert " << n << " keys..." << std::endl;
    for (int i = 0; i < n; i++)
    {
        std::string key_str = "key-" + std::to_string(i);
        MyKey key;
        key.SetString(key_str.c_str());
        int value = i * 100;

        tree.Insert(key, value);
        std::cout << "Inserted: " << key << " -> " << value << std::endl;
    }

    // 2. Search them back
    std::cout << "\nSearch " << n << " keys..." << std::endl;
    for (int i = 0; i < n; i++)
    {
        std::string key_str = "key-" + std::to_string(i);
        MyKey key;
        key.SetString(key_str.c_str());

        std::vector<MyValue> result;
        bool found = tree.GetValue(key, result);
        if (found)
        {
            std::cout << "Found: " << key << " -> " << result[0] << std::endl;
        }
        else
        {
            std::cout << "NOT FOUND: " << key << std::endl;
        }
    }

    delete bpm;
    delete disk;
    return 0;
}
