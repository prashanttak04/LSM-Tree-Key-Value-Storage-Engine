#include "common.h"
#include "manifest.h"
#include "sstable.h"
#include "compaction.h"
#include <cassert>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

using namespace lsm;

void TestManifestBasic() {
    std::cout << "Running TestManifestBasic..." << std::endl;
    const std::string db_path = "test_db_manifest";
    ::mkdir(db_path.c_str(), 0755);
    
    // Remove manifest if exists
    ::unlink((db_path + "/MANIFEST").c_str());

    {
        Manifest manifest(db_path);
        assert(manifest.Load().ok());
        assert(manifest.GetNextFileNumber() == 1);

        manifest.AddFile(0, manifest.NextFileNumber(), 100, "a", "c");
        manifest.AddFile(0, manifest.NextFileNumber(), 200, "b", "d");
        manifest.AddFile(1, manifest.NextFileNumber(), 300, "x", "z");
        assert(manifest.Save().ok());
    }

    // Load back
    {
        Manifest manifest(db_path);
        assert(manifest.Load().ok());
        assert(manifest.GetNextFileNumber() == 4);

        const auto& l0 = manifest.GetLevelFiles(0);
        assert(l0.size() == 2);
        assert(l0[0].file_number == 1 && l0[0].file_size == 100 && l0[0].smallest_key == "a");
        assert(l0[1].file_number == 2 && l0[1].file_size == 200 && l0[1].smallest_key == "b");

        const auto& l1 = manifest.GetLevelFiles(1);
        assert(l1.size() == 1);
        assert(l1[0].file_number == 3 && l1[0].file_size == 300 && l1[0].smallest_key == "x");
    }

    // Clean up
    ::unlink((db_path + "/MANIFEST").c_str());
    ::rmdir(db_path.c_str());
    std::cout << "TestManifestBasic PASSED!" << std::endl;
}

void TestCompactionBasic() {
    std::cout << "Running TestCompactionBasic..." << std::endl;
    const std::string db_path = "test_db_compact";
    ::mkdir(db_path.c_str(), 0755);

    // Reset Manifest
    ::unlink((db_path + "/MANIFEST").c_str());

    Manifest manifest(db_path);
    assert(manifest.Load().ok());

    // Write L0 File 1: key_1 -> val1, key_3 -> val3_old
    uint64_t f1 = manifest.NextFileNumber();
    {
        SSTableWriter writer(db_path + "/" + std::to_string(f1) + ".sst");
        assert(writer.Append("key_1", Entry{"val1", kTypeValue}).ok());
        assert(writer.Append("key_3", Entry{"val3_old", kTypeValue}).ok());
        assert(writer.Finish().ok());
    }
    manifest.AddFile(0, f1, 100, "key_1", "key_3");

    // Write L0 File 2: key_2 -> val2, key_3 -> val3_new
    uint64_t f2 = manifest.NextFileNumber();
    {
        SSTableWriter writer(db_path + "/" + std::to_string(f2) + ".sst");
        assert(writer.Append("key_2", Entry{"val2", kTypeValue}).ok());
        assert(writer.Append("key_3", Entry{"val3_new", kTypeValue}).ok());
        assert(writer.Finish().ok());
    }
    manifest.AddFile(0, f2, 100, "key_2", "key_3");

    // Write L0 File 3: delete key_1, key_4 -> val4
    uint64_t f3 = manifest.NextFileNumber();
    {
        SSTableWriter writer(db_path + "/" + std::to_string(f3) + ".sst");
        assert(writer.Append("key_1", Entry{"", kTypeDeletion}).ok());
        assert(writer.Append("key_4", Entry{"val4", kTypeValue}).ok());
        assert(writer.Finish().ok());
    }
    manifest.AddFile(0, f3, 100, "key_1", "key_4");

    assert(manifest.Save().ok());

    // Run Compaction!
    Status s = Compaction::Run(db_path, &manifest);
    assert(s.ok());

    // Verify Manifest State
    assert(manifest.GetLevelFiles(0).empty());
    const auto& l1 = manifest.GetLevelFiles(1);
    assert(l1.size() == 1);
    uint64_t l1_file_num = l1[0].file_number;

    // Verify contents of the new L1 file
    SSTableReader reader(db_path + "/" + std::to_string(l1_file_num) + ".sst");
    assert(reader.Open().ok());

    Entry entry;
    // key_1 was deleted, so it should NOT be found (evicted from bottom level)
    assert(!reader.Get("key_1", &entry));

    // key_2 should have "val2"
    assert(reader.Get("key_2", &entry));
    assert(entry.value == "val2" && entry.type == kTypeValue);

    // key_3 should have "val3_new"
    assert(reader.Get("key_3", &entry));
    assert(entry.value == "val3_new" && entry.type == kTypeValue);

    // key_4 should have "val4"
    assert(reader.Get("key_4", &entry));
    assert(entry.value == "val4" && entry.type == kTypeValue);

    // Clean up files
    ::unlink((db_path + "/" + std::to_string(l1_file_num) + ".sst").c_str());
    ::unlink((db_path + "/MANIFEST").c_str());
    ::rmdir(db_path.c_str());

    std::cout << "TestCompactionBasic PASSED!" << std::endl;
}

int main() {
    TestManifestBasic();
    TestCompactionBasic();
    std::cout << "All Phase 3 Tests PASSED!" << std::endl;
    return 0;
}
