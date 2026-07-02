#ifndef LSM_WAL_H
#define LSM_WAL_H

#include "common.h"
#include "skiplist.h"

namespace lsm {

// WAL Record format:
// [checksum: 4 bytes] [type: 1 byte] [key_len: 4 bytes] [key] [val_len: 4 bytes] [value]
class WALWriter {
public:
    explicit WALWriter(const std::string& filepath);
    ~WALWriter();

    // Disable copy
    WALWriter(const WALWriter&) = delete;
    WALWriter& operator=(const WALWriter&) = delete;

    Status Append(const std::string& key, const std::string& value, ValueType type);
    Status Sync();
    void Close();

private:
    int fd_;
    std::string filepath_;
};

class WALReader {
public:
    explicit WALReader(const std::string& filepath);
    ~WALReader();

    // Recover contents from WAL file into MemTable
    Status Recover(SkipList& memtable);

private:
    int fd_;
    std::string filepath_;
};

} // namespace lsm

#endif // LSM_WAL_H
