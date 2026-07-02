#include "manifest.h"
#include <cstdio>
#include <algorithm>
#include <sstream>

namespace lsm {

Manifest::Manifest(const std::string& db_path) 
    : db_path_(db_path), next_file_number_(1) {}

Manifest::~Manifest() {}

Status Manifest::Load() {
    std::string manifest_path = db_path_ + "/MANIFEST";
    std::ifstream file(manifest_path, std::ios::binary | std::ios::in);
    if (!file.is_open()) {
        // Not found, starting fresh is normal
        next_file_number_ = 1;
        return Status::OK();
    }

    // Read full contents
    std::string buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    const char* ptr = buf.data();
    const char* limit = buf.data() + buf.size();

    uint64_t next_file_num = 0;
    uint32_t num_levels = 0;

    if (!GetFixed64(&ptr, limit, &next_file_num) ||
        !GetFixed32(&ptr, limit, &num_levels)) {
        return Status::Corruption("Corrupted Manifest header");
    }

    next_file_number_ = next_file_num;

    for (uint32_t i = 0; i < num_levels; ++i) {
        uint32_t level_num = 0;
        uint32_t num_files = 0;
        if (!GetFixed32(&ptr, limit, &level_num) ||
            !GetFixed32(&ptr, limit, &num_files)) {
            return Status::Corruption("Corrupted Manifest level markers");
        }

        if (level_num >= kMaxLevels) {
            return Status::Corruption("Manifest level out of bounds");
        }

        levels_[level_num].clear();
        for (uint32_t f = 0; f < num_files; ++f) {
            FileMetaData meta;
            if (!GetFixed64(&ptr, limit, &meta.file_number) ||
                !GetFixed64(&ptr, limit, &meta.file_size) ||
                !GetLengthPrefixedSlice(&ptr, limit, &meta.smallest_key) ||
                !GetLengthPrefixedSlice(&ptr, limit, &meta.largest_key)) {
                return Status::Corruption("Corrupted Manifest file entry metadata");
            }
            levels_[level_num].push_back(meta);
        }

        // Ensure non-L0 files are sorted by their key ranges
        if (level_num > 0) {
            std::sort(levels_[level_num].begin(), levels_[level_num].end(), 
                [](const FileMetaData& a, const FileMetaData& b) {
                    return a.smallest_key < b.smallest_key;
                });
        }
    }

    return Status::OK();
}

Status Manifest::Save() {
    std::string manifest_path = db_path_ + "/MANIFEST";
    std::string tmp_path = manifest_path + ".tmp";

    std::ofstream file(tmp_path, std::ios::binary | std::ios::out);
    if (!file.is_open()) {
        return Status::IOError("Failed to open MANIFEST.tmp for writing");
    }

    std::string buf;
    PutFixed64(&buf, next_file_number_);
    PutFixed32(&buf, static_cast<uint32_t>(kMaxLevels));

    for (int i = 0; i < kMaxLevels; ++i) {
        PutFixed32(&buf, static_cast<uint32_t>(i));
        PutFixed32(&buf, static_cast<uint32_t>(levels_[i].size()));
        
        for (const auto& meta : levels_[i]) {
            PutFixed64(&buf, meta.file_number);
            PutFixed64(&buf, meta.file_size);
            PutLengthPrefixedSlice(&buf, meta.smallest_key);
            PutLengthPrefixedSlice(&buf, meta.largest_key);
        }
    }

    file.write(buf.data(), buf.size());
    if (file.fail()) {
        file.close();
        return Status::IOError("Failed to write buffer to MANIFEST.tmp");
    }
    
    file.close();

    // Atomic POSIX rename replaces manifest file atomically
    if (std::rename(tmp_path.c_str(), manifest_path.c_str()) != 0) {
        return Status::IOError("Failed to rename MANIFEST.tmp to MANIFEST");
    }

    return Status::OK();
}

void Manifest::AddFile(int level, uint64_t file_number, uint64_t file_size, 
                       const std::string& smallest_key, const std::string& largest_key) {
    if (level < 0 || level >= kMaxLevels) return;

    FileMetaData meta{file_number, file_size, smallest_key, largest_key};
    levels_[level].push_back(meta);

    // Keep levels L1 and above sorted by smallest_key for fast binary search ranges
    if (level > 0) {
        std::sort(levels_[level].begin(), levels_[level].end(), 
            [](const FileMetaData& a, const FileMetaData& b) {
                return a.smallest_key < b.smallest_key;
            });
    }
}

void Manifest::RemoveFile(int level, uint64_t file_number) {
    if (level < 0 || level >= kMaxLevels) return;

    auto& files = levels_[level];
    files.erase(
        std::remove_if(files.begin(), files.end(), 
            [file_number](const FileMetaData& m) {
                return m.file_number == file_number;
            }), 
        files.end());
}

std::string Manifest::GetStats() const {
    std::stringstream ss;
    ss << "--- LSM Levels statistics ---\n";
    for (int i = 0; i < kMaxLevels; ++i) {
        ss << "Level " << i << ": " << levels_[i].size() << " files [";
        for (size_t f = 0; f < levels_[i].size(); ++f) {
            ss << "#" << levels_[i][f].file_number << " (" << levels_[i][f].file_size << "B)";
            if (f + 1 < levels_[i].size()) ss << ", ";
        }
        ss << "]\n";
    }
    ss << "Next File Number: " << next_file_number_ << "\n";
    return ss.str();
}

} // namespace lsm
