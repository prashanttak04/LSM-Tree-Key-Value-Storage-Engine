#ifndef LSM_MANIFEST_H
#define LSM_MANIFEST_H

#include "common.h"
#include <vector>
#include <string>

namespace lsm {

struct FileMetaData {
    uint64_t file_number;
    uint64_t file_size;
    std::string smallest_key;
    std::string largest_key;
};

class Manifest {
public:
    static constexpr int kMaxLevels = 4;

    explicit Manifest(const std::string& db_path);
    ~Manifest();

    // Load active state from MANIFEST file. Returns true if loaded successfully.
    Status Load();

    // Save current state atomically to MANIFEST file (snapshot write + rename)
    Status Save();

    // Add a file metadata to a level
    void AddFile(int level, uint64_t file_number, uint64_t file_size, 
                 const std::string& smallest_key, const std::string& largest_key);

    // Remove a file from a level
    void RemoveFile(int level, uint64_t file_number);

    // Get next available file number and increment the counter
    uint64_t NextFileNumber() { return next_file_number_++; }
    
    uint64_t GetNextFileNumber() const { return next_file_number_; }

    const std::vector<FileMetaData>& GetLevelFiles(int level) const {
        if (level < 0 || level >= kMaxLevels) {
            throw std::out_of_range("Invalid level index");
        }
        return levels_[level];
    }

    std::string GetStats() const;

private:
    std::string db_path_;
    uint64_t next_file_number_;
    std::vector<FileMetaData> levels_[kMaxLevels];
};

} // namespace lsm

#endif // LSM_MANIFEST_H
