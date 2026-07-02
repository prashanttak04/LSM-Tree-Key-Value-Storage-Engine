#ifndef LSM_SSTABLE_H
#define LSM_SSTABLE_H

#include "common.h"
#include "bloom.h"
#include <fstream>
#include <vector>

namespace lsm {

// SSTable File Layout:
// +--------------------------+
// | Data Block               | <- Series of [key_len][val_len][type][key][value]
// +--------------------------+
// | Index Block              | <- Series of [key_len][offset][key]
// +--------------------------+
// | Bloom Filter Block       | <- [num_hashes: 4 bytes] [len: 4 bytes] [filter bytes]
// +--------------------------+
// | Footer (Fixed 40 bytes)  | <- [index_offset: 8 bytes][index_len: 8 bytes]
// |                          |    [bloom_offset: 8 bytes][bloom_len: 8 bytes]
// |                          |    [magic: 8 bytes]
// +--------------------------+

class SSTableWriter {
public:
    explicit SSTableWriter(const std::string& filepath);
    ~SSTableWriter();

    // Append a key-value record to the SSTable. Keys must be appended in sorted order!
    Status Append(const std::string& key, const Entry& entry);

    // Finalize the SSTable by writing the index, bloom filter, and footer
    Status Finish();

    uint64_t GetOffset() const { return offset_; }

private:
    std::ofstream file_;
    std::string filepath_;
    uint64_t offset_;
    
    // Accumulate keys for Bloom Filter generation
    std::vector<std::string> keys_;
    
    // Index entries: [key, file_offset]
    std::vector<std::pair<std::string, uint64_t>> index_;
    
    size_t record_count_;
    const size_t sparse_index_interval_; // Create index record every N keys
};

class SSTableReader {
public:
    explicit SSTableReader(const std::string& filepath);
    ~SSTableReader();

    // Open file, read footer, load index and bloom filter into memory
    Status Open();

    // Retrieve value by key. Returns true if key found (even if deleted/tombstone).
    bool Get(const std::string& key, Entry* entry);

    // Scan a range of keys [start_key, end_key]. Results appended to vector.
    void Scan(const std::string& start_key, const std::string& end_key, 
              std::vector<std::pair<std::string, Entry>>& results);

    const std::string& GetFilepath() const { return filepath_; }

private:
    std::string filepath_;
    int fd_;
    uint64_t file_size_;

    // In-memory Index
    std::vector<std::pair<std::string, uint64_t>> index_;
    
    // In-memory Bloom Filter
    std::unique_ptr<BloomFilter> bloom_;
};

// Iterator for sequential streaming of SSTable records
class SSTableIterator {
public:
    explicit SSTableIterator(const std::string& filepath);
    ~SSTableIterator();

    bool Valid() const { return valid_; }
    void Next();
    const std::string& key() const { return curr_key_; }
    const Entry& entry() const { return curr_entry_; }

private:
    int fd_;
    uint64_t curr_offset_;
    uint64_t data_end_offset_;
    std::string curr_key_;
    Entry curr_entry_;
    bool valid_;
};

} // namespace lsm

#endif // LSM_SSTABLE_H
