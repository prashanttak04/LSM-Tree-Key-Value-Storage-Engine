#include "common.h"
#include "skiplist.h"
#include "wal.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <thread>
#include <unistd.h>

using namespace lsm;

void TestSkipListBasic() {
    std::cout << "Running TestSkipListBasic..." << std::endl;
    SkipList sl;
    
    assert(sl.size() == 0);
    sl.Insert("key2", Entry{"val2", kTypeValue});
    sl.Insert("key1", Entry{"val1", kTypeValue});
    sl.Insert("key3", Entry{"val3", kTypeValue});
    assert(sl.size() == 3);

    Entry ent;
    assert(sl.Find("key1", &ent) && ent.value == "val1" && ent.type == kTypeValue);
    assert(sl.Find("key2", &ent) && ent.value == "val2" && ent.type == kTypeValue);
    assert(sl.Find("key3", &ent) && ent.value == "val3" && ent.type == kTypeValue);
    assert(!sl.Find("key4", &ent));

    // Update test
    sl.Insert("key2", Entry{"val2_new", kTypeValue});
    assert(sl.Find("key2", &ent) && ent.value == "val2_new" && ent.type == kTypeValue);
    assert(sl.size() == 3);

    // Iterator test
    auto it = sl.Begin();
    assert(it.Valid());
    assert(it.key() == "key1");
    it.Next();
    assert(it.Valid());
    assert(it.key() == "key2");
    it.Next();
    assert(it.Valid());
    assert(it.key() == "key3");
    it.Next();
    assert(!it.Valid());

    std::cout << "TestSkipListBasic PASSED!" << std::endl;
}

void TestSkipListConcurrency() {
    std::cout << "Running TestSkipListConcurrency..." << std::endl;
    SkipList sl;
    const int kNumThreads = 4;
    const int kOpsPerThread = 1000;

    std::vector<std::thread> threads;
    // Writer threads
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&sl, t]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                sl.Insert(std::to_string(t * kOpsPerThread + i), Entry{std::to_string(i), kTypeValue});
            }
        });
    }

    // Reader threads
    std::vector<std::thread> readers;
    for (int t = 0; t < kNumThreads; ++t) {
        readers.emplace_back([&sl]() {
            Entry ent;
            for (int i = 0; i < kOpsPerThread * kNumThreads; ++i) {
                sl.Find(std::to_string(i), &ent);
            }
        });
    }

    for (auto& t : threads) t.join();
    for (auto& t : readers) t.join();

    assert(sl.size() == kNumThreads * kOpsPerThread);
    std::cout << "TestSkipListConcurrency PASSED!" << std::endl;
}

void TestWALBasic() {
    std::cout << "Running TestWALBasic..." << std::endl;
    const std::string wal_path = "test.wal";
    // Remove if exists
    ::unlink(wal_path.c_str());

    {
        WALWriter writer(wal_path);
        assert(writer.Append("key1", "val1", kTypeValue).ok());
        assert(writer.Append("key2", "val2", kTypeValue).ok());
        assert(writer.Append("key3", "", kTypeDeletion).ok());
        assert(writer.Sync().ok());
    }

    // Recover
    SkipList sl;
    {
        WALReader reader(wal_path);
        Status s = reader.Recover(sl);
        assert(s.ok());
    }

    assert(sl.size() == 3);
    Entry ent;
    assert(sl.Find("key1", &ent) && ent.value == "val1" && ent.type == kTypeValue);
    assert(sl.Find("key2", &ent) && ent.value == "val2" && ent.type == kTypeValue);
    assert(sl.Find("key3", &ent) && ent.value == "" && ent.type == kTypeDeletion);

    // Clean up
    ::unlink(wal_path.c_str());
    std::cout << "TestWALBasic PASSED!" << std::endl;
}

int main() {
    TestSkipListBasic();
    TestSkipListConcurrency();
    TestWALBasic();
    std::cout << "All Phase 1 Tests PASSED!" << std::endl;
    return 0;
}
