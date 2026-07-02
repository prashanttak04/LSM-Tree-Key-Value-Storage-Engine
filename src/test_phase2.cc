#include "common.h"
#include "bloom.h"
#include "sstable.h"
#include <cassert>
#include <iostream>
#include <unistd.h>

using namespace lsm;

void TestBloomFilter() {
    std::cout << "Running TestBloomFilter..." << std::endl;
    
    // Create filter for 10 keys
    BloomFilter filter(10, 0.01);
    
    filter.Add("apple");
    filter.Add("banana");
    filter.Add("cherry");

    assert(filter.Contains("apple"));
    assert(filter.Contains("banana"));
    assert(filter.Contains("cherry"));
    
    // Probabilistic check (should be false for almost all non-added strings)
    assert(!filter.Contains("durian"));
    assert(!filter.Contains("elderberry"));

    std::cout << "TestBloomFilter PASSED!" << std::endl;
}

void TestSSTableBasic() {
    std::cout << "Running TestSSTableBasic..." << std::endl;
    const std::string sst_path = "test.sst";
    ::unlink(sst_path.c_str());

    // 1. Write sorted entries
    {
        SSTableWriter writer(sst_path);
        // Keys must be added in sorted order
        for (int i = 0; i < 100; ++i) {
            std::string key = std::string("key_") + (i < 10 ? "0" : "") + std::to_string(i); // e.g. key_00, key_01 ... key_99
            Entry entry{"value_" + std::to_string(i), kTypeValue};
            assert(writer.Append(key, entry).ok());
        }
        assert(writer.Finish().ok());
    }

    // 2. Read entries back
    {
        SSTableReader reader(sst_path);
        assert(reader.Open().ok());

        // Exact lookups
        Entry entry;
        for (int i = 0; i < 100; ++i) {
            std::string key = std::string("key_") + (i < 10 ? "0" : "") + std::to_string(i);
            assert(reader.Get(key, &entry));
            assert(entry.value == "value_" + std::to_string(i));
            assert(entry.type == kTypeValue);
        }

        // Non-existent key lookup
        assert(!reader.Get("key_101", &entry));
        assert(!reader.Get("key_00a", &entry));
        assert(!reader.Get("abc", &entry));

        // Scan test
        std::vector<std::pair<std::string, Entry>> scan_results;
        reader.Scan("key_25", "key_35", scan_results);
        assert(scan_results.size() == 11); // key_25 to key_35 inclusive
        assert(scan_results[0].first == "key_25" && scan_results[0].second.value == "value_25");
        assert(scan_results[10].first == "key_35" && scan_results[10].second.value == "value_35");
    }

    // Clean up
    ::unlink(sst_path.c_str());
    std::cout << "TestSSTableBasic PASSED!" << std::endl;
}

int main() {
    TestBloomFilter();
    TestSSTableBasic();
    std::cout << "All Phase 2 Tests PASSED!" << std::endl;
    return 0;
}
