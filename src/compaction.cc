#include "compaction.h"
#include "sstable.h"
#include <iostream>
#include <algorithm>
#include <memory>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace lsm {

// Max size of a single SSTable file (2MB)
static constexpr uint64_t kMaxSSTableSize = 2 * 1024 * 1024;

struct MergeIterator {
    std::unique_ptr<SSTableIterator> iter;
    uint64_t file_number;
    int level; // 0 for L0, 1 for L1
};

Status Compaction::Run(const std::string& db_path, Manifest* manifest) {
    std::vector<FileMetaData> l0_files = manifest->GetLevelFiles(0);
    if (l0_files.empty()) {
        return Status::OK(); // Nothing to compact
    }

    // 1. Determine key range for all Level 0 files
    std::string min_key = l0_files[0].smallest_key;
    std::string max_key = l0_files[0].largest_key;
    for (const auto& f : l0_files) {
        if (f.smallest_key < min_key) min_key = f.smallest_key;
        if (f.largest_key > max_key) max_key = f.largest_key;
    }

    // 2. Identify overlapping files in Level 1
    std::vector<FileMetaData> l1_files = manifest->GetLevelFiles(1);
    std::vector<FileMetaData> overlapping_l1;
    for (const auto& f : l1_files) {
        if (f.smallest_key <= max_key && f.largest_key >= min_key) {
            overlapping_l1.push_back(f);
        }
    }

    // 3. Initialize streaming iterators for all inputs
    std::vector<MergeIterator> inputs;
    for (const auto& f : l0_files) {
        std::string path = db_path + "/" + std::to_string(f.file_number) + ".sst";
        auto it = std::make_unique<SSTableIterator>(path);
        if (it->Valid()) {
            inputs.push_back({std::move(it), f.file_number, 0});
        }
    }
    for (const auto& f : overlapping_l1) {
        std::string path = db_path + "/" + std::to_string(f.file_number) + ".sst";
        auto it = std::make_unique<SSTableIterator>(path);
        if (it->Valid()) {
            inputs.push_back({std::move(it), f.file_number, 1});
        }
    }

    // 4. Multi-way merge sort loop
    std::unique_ptr<SSTableWriter> writer = nullptr;
    std::vector<FileMetaData> new_l1_files;

    uint64_t current_file_number = 0;
    std::string current_file_path = "";
    std::string smallest_key_in_file = "";
    std::string largest_key_in_file = "";

    auto start_new_sstable = [&]() -> Status {
        current_file_number = manifest->NextFileNumber();
        current_file_path = db_path + "/" + std::to_string(current_file_number) + ".sst";
        writer = std::make_unique<SSTableWriter>(current_file_path);
        smallest_key_in_file = "";
        largest_key_in_file = "";
        return Status::OK();
    };

    if (!inputs.empty()) {
        Status s = start_new_sstable();
        if (!s.ok()) return s;
    }

    while (true) {
        // Find inputs that are valid
        std::vector<size_t> valid_indices;
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].iter->Valid()) {
                valid_indices.push_back(i);
            }
        }

        if (valid_indices.empty()) {
            break; // No more records to merge
        }

        // Find the smallest key among all valid inputs
        std::string smallest_key = inputs[valid_indices[0]].iter->key();
        for (size_t idx : valid_indices) {
            if (inputs[idx].iter->key() < smallest_key) {
                smallest_key = inputs[idx].iter->key();
            }
        }

        // Select the newest entry corresponding to the smallest key.
        // Rule: 
        // 1. Level 0 is newer than Level 1.
        // 2. Higher file number is newer.
        size_t best_idx = valid_indices[0]; // dummy initialization
        bool best_set = false;

        for (size_t idx : valid_indices) {
            if (inputs[idx].iter->key() == smallest_key) {
                if (!best_set) {
                    best_idx = idx;
                    best_set = true;
                } else {
                    // Compare age:
                    // inputs[idx] vs inputs[best_idx]
                    bool idx_is_newer = false;
                    if (inputs[idx].level < inputs[best_idx].level) {
                        idx_is_newer = true; // L0 is newer than L1
                    } else if (inputs[idx].level == inputs[best_idx].level) {
                        if (inputs[idx].file_number > inputs[best_idx].file_number) {
                            idx_is_newer = true; // Newer file number is newer
                        }
                    }
                    if (idx_is_newer) {
                        best_idx = idx;
                    }
                }
            }
        }

        // Save selected key and entry
        std::string key_to_write = smallest_key;
        Entry entry_to_write = inputs[best_idx].iter->entry();

        // Advance all iterators matching this smallest key (evicting stale versions)
        for (size_t idx : valid_indices) {
            if (inputs[idx].iter->key() == smallest_key) {
                inputs[idx].iter->Next();
            }
        }

        // Tombstone eviction optimization:
        // If entry is a deletion AND we are writing to the lowest level (Level 1),
        // we can discard the deletion record because there are no lower levels (L2, etc.) to mask.
        if (entry_to_write.type == kTypeDeletion) {
            continue;
        }

        // Check if we need to roll over to a new SSTable (max size exceeded)
        if (writer->GetOffset() >= kMaxSSTableSize) {
            Status s = writer->Finish();
            if (!s.ok()) return s;

            // Get file size
            struct stat st;
            uint64_t size = 0;
            if (::stat(current_file_path.c_str(), &st) == 0) {
                size = st.st_size;
            }
            new_l1_files.push_back({current_file_number, size, smallest_key_in_file, largest_key_in_file});

            s = start_new_sstable();
            if (!s.ok()) return s;
        }

        // Write to current SSTable
        Status s = writer->Append(key_to_write, entry_to_write);
        if (!s.ok()) return s;

        if (smallest_key_in_file.empty()) {
            smallest_key_in_file = key_to_write;
        }
        largest_key_in_file = key_to_write;
    }

    // Finish writing the last SSTable if any data was written
    if (writer && !smallest_key_in_file.empty()) {
        Status s = writer->Finish();
        if (!s.ok()) return s;

        struct stat st;
        uint64_t size = 0;
        if (::stat(current_file_path.c_str(), &st) == 0) {
            size = st.st_size;
        }
        new_l1_files.push_back({current_file_number, size, smallest_key_in_file, largest_key_in_file});
    } else if (writer) {
        // Empty compaction output, remove empty file
        writer = nullptr;
        ::unlink(current_file_path.c_str());
    }

    // 5. Update Manifest
    // Remove old Level 0 files
    for (const auto& f : l0_files) {
        manifest->RemoveFile(0, f.file_number);
    }
    // Remove old Level 1 files
    for (const auto& f : overlapping_l1) {
        manifest->RemoveFile(1, f.file_number);
    }
    // Add new Level 1 files
    for (const auto& f : new_l1_files) {
        manifest->AddFile(1, f.file_number, f.file_size, f.smallest_key, f.largest_key);
    }

    Status s = manifest->Save();
    if (!s.ok()) return s;

    // 6. Delete obsolete files from disk
    for (const auto& f : l0_files) {
        std::string path = db_path + "/" + std::to_string(f.file_number) + ".sst";
        ::unlink(path.c_str());
    }
    for (const auto& f : overlapping_l1) {
        std::string path = db_path + "/" + std::to_string(f.file_number) + ".sst";
        ::unlink(path.c_str());
    }

    return Status::OK();
}

} // namespace lsm
