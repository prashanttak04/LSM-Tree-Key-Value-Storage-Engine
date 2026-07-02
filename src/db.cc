#include "db.h"
#include "compaction.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

namespace lsm {

DB::DB(const std::string& db_path)
    : db_path_(db_path),
      active_memtable_(nullptr),
      frozen_memtable_(nullptr),
      active_wal_(nullptr),
      shutting_down_(false),
      flush_requested_(false),
      max_memtable_size_(2 * 1024 * 1024), // 2MB trigger size
      l0_compaction_trigger_(4) { // Trigger compaction when L0 >= 4 files
}

DB::~DB() {
    // 1. Signal shutdown to background thread
    {
        std::unique_lock<std::mutex> lock(bg_mutex_);
        shutting_down_ = true;
        bg_cv_.notify_all();
    }
    if (bg_thread_.joinable()) {
        bg_thread_.join();
    }

    // 2. Flush active memtable on clean shutdown
    if (active_memtable_ && active_memtable_->size() > 0) {
        uint64_t file_num = manifest_->NextFileNumber();
        FileMetaData meta;
        if (WriteLevel0Table(active_memtable_, file_num, &meta).ok()) {
            manifest_->AddFile(0, file_num, meta.file_size, meta.smallest_key, meta.largest_key);
            manifest_->Save();
            
            // Delete active WAL
            if (active_wal_) {
                active_wal_->Close();
                delete active_wal_;
                active_wal_ = nullptr;
            }
            ::unlink((db_path_ + "/active.wal").c_str());
        }
    }

    // 3. Clear memory
    delete active_memtable_;
    delete frozen_memtable_;
    if (active_wal_) {
        delete active_wal_;
    }
}

Status DB::Open(const std::string& db_path, DB** dbptr) {
    *dbptr = nullptr;
    
    // Create DB directory if not exists
    ::mkdir(db_path.c_str(), 0755);

    DB* db = new DB(db_path);
    db->manifest_ = std::make_unique<Manifest>(db_path);
    
    Status s = db->manifest_->Load();
    if (!s.ok()) {
        delete db;
        return s;
    }

    db->active_memtable_ = new SkipList();

    // Recover any logs on disk
    s = db->Recover();
    if (!s.ok()) {
        delete db;
        return s;
    }

    // Open active WAL writer
    db->active_wal_ = new WALWriter(db_path + "/active.wal");

    // Load readers for files listed in manifest
    s = db->LoadSSTableReaders();
    if (!s.ok()) {
        delete db;
        return s;
    }

    // Spin up background worker thread
    db->bg_thread_ = std::thread(&DB::BackgroundWorker, db);

    *dbptr = db;
    return Status::OK();
}

Status DB::Put(const std::string& key, const std::string& value) {
    std::unique_lock<std::shared_mutex> db_lock(db_mutex_);

    // If a flush is currently in progress, we wait
    while (frozen_memtable_ != nullptr) {
        db_lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        db_lock.lock();
    }

    // Trigger flush if active memtable exceeds threshold size
    if (active_memtable_->EstimateMemoryUsage() >= max_memtable_size_) {
        Status s = MaybeScheduleFlush();
        if (!s.ok()) return s;
    }

    // Write to WAL first
    Status s = active_wal_->Append(key, value, kTypeValue);
    if (!s.ok()) return s;
    s = active_wal_->Sync();
    if (!s.ok()) return s;

    // Insert to MemTable
    active_memtable_->Insert(key, Entry{value, kTypeValue});
    return Status::OK();
}

Status DB::Delete(const std::string& key) {
    std::unique_lock<std::shared_mutex> db_lock(db_mutex_);

    while (frozen_memtable_ != nullptr) {
        db_lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        db_lock.lock();
    }

    if (active_memtable_->EstimateMemoryUsage() >= max_memtable_size_) {
        Status s = MaybeScheduleFlush();
        if (!s.ok()) return s;
    }

    // Write deletion to WAL
    Status s = active_wal_->Append(key, "", kTypeDeletion);
    if (!s.ok()) return s;
    s = active_wal_->Sync();
    if (!s.ok()) return s;

    // Write deletion to MemTable (Tombstone)
    active_memtable_->Insert(key, Entry{"", kTypeDeletion});
    return Status::OK();
}

Status DB::Get(const std::string& key, std::string* value) {
    std::shared_lock<std::shared_mutex> db_lock(db_mutex_);

    // 1. Search Active MemTable
    Entry entry;
    if (active_memtable_->Find(key, &entry)) {
        if (entry.type == kTypeDeletion) {
            return Status::NotFound("Key was deleted");
        }
        *value = entry.value;
        return Status::OK();
    }

    // 2. Search Frozen MemTable (if exists)
    if (frozen_memtable_ && frozen_memtable_->Find(key, &entry)) {
        if (entry.type == kTypeDeletion) {
            return Status::NotFound("Key was deleted");
        }
        *value = entry.value;
        return Status::OK();
    }

    // Read unlock DB lock to avoid holding lock during Disk I/O
    db_lock.unlock();

    std::shared_lock<std::shared_mutex> readers_lock(readers_mutex_);

    // 3. Search Level 0 SSTables (from newest to oldest: highest file number first)
    std::vector<FileMetaData> l0_files = manifest_->GetLevelFiles(0);
    std::sort(l0_files.begin(), l0_files.end(), [](const FileMetaData& a, const FileMetaData& b) {
        return a.file_number > b.file_number;
    });

    for (const auto& meta : l0_files) {
        auto it = sstable_readers_.find(meta.file_number);
        if (it != sstable_readers_.end()) {
            if (it->second->Get(key, &entry)) {
                if (entry.type == kTypeDeletion) {
                    return Status::NotFound("Key was deleted");
                }
                *value = entry.value;
                return Status::OK();
            }
        }
    }

    // 4. Search Level 1 SSTables (non-overlapping sorted keys: binary search the files)
    const auto& l1_files = manifest_->GetLevelFiles(1);
    auto it = std::upper_bound(l1_files.begin(), l1_files.end(), key, 
        [](const std::string& k, const FileMetaData& element) {
            return k < element.smallest_key;
        });

    if (it != l1_files.begin()) {
        --it;
        if (key >= it->smallest_key && key <= it->largest_key) {
            auto r_it = sstable_readers_.find(it->file_number);
            if (r_it != sstable_readers_.end()) {
                if (r_it->second->Get(key, &entry)) {
                    if (entry.type == kTypeDeletion) {
                        return Status::NotFound("Key was deleted");
                    }
                    *value = entry.value;
                    return Status::OK();
                }
            }
        }
    }

    return Status::NotFound("Key not found in database");
}

Status DB::Scan(const std::string& start_key, const std::string& end_key, 
                std::vector<std::pair<std::string, std::string>>& results) {
    std::shared_lock<std::shared_mutex> db_lock(db_mutex_);
    std::shared_lock<std::shared_mutex> readers_lock(readers_mutex_);

    // map to automatically sort keys and overwrite older values with newer versions
    std::map<std::string, Entry> merged_map;

    // 1. Scan Level 1 SSTables (Oldest)
    for (const auto& meta : manifest_->GetLevelFiles(1)) {
        // Skip files that definitely don't overlap with scan range
        if (meta.largest_key < start_key || meta.smallest_key > end_key) {
            continue;
        }
        auto r_it = sstable_readers_.find(meta.file_number);
        if (r_it != sstable_readers_.end()) {
            std::vector<std::pair<std::string, Entry>> temp_results;
            r_it->second->Scan(start_key, end_key, temp_results);
            for (const auto& pair : temp_results) {
                merged_map[pair.first] = pair.second;
            }
        }
    }

    // 2. Scan Level 0 SSTables (Newer than L1, oldest L0 file first)
    std::vector<FileMetaData> l0_files = manifest_->GetLevelFiles(0);
    std::sort(l0_files.begin(), l0_files.end(), [](const FileMetaData& a, const FileMetaData& b) {
        return a.file_number < b.file_number;
    });

    for (const auto& meta : l0_files) {
        if (meta.largest_key < start_key || meta.smallest_key > end_key) {
            continue;
        }
        auto r_it = sstable_readers_.find(meta.file_number);
        if (r_it != sstable_readers_.end()) {
            std::vector<std::pair<std::string, Entry>> temp_results;
            r_it->second->Scan(start_key, end_key, temp_results);
            for (const auto& pair : temp_results) {
                merged_map[pair.first] = pair.second;
            }
        }
    }

    // 3. Scan Frozen MemTable (if exists, newer than L0 files)
    if (frozen_memtable_) {
        auto it = frozen_memtable_->Begin();
        while (it.Valid()) {
            if (it.key() >= start_key && it.key() <= end_key) {
                merged_map[it.key()] = it.entry();
            } else if (it.key() > end_key) {
                break;
            }
            it.Next();
        }
    }

    // 4. Scan Active MemTable (Newest)
    if (active_memtable_) {
        auto it = active_memtable_->Begin();
        while (it.Valid()) {
            if (it.key() >= start_key && it.key() <= end_key) {
                merged_map[it.key()] = it.entry();
            } else if (it.key() > end_key) {
                break;
            }
            it.Next();
        }
    }

    // Extract valid non-deleted entries
    results.clear();
    for (const auto& pair : merged_map) {
        if (pair.second.type == kTypeValue) {
            results.push_back({pair.first, pair.second.value});
        }
    }

    return Status::OK();
}

Status DB::MaybeScheduleFlush() {
    active_wal_->Close();
    
    // Rename active.wal to frozen.wal
    std::string active_wal_path = db_path_ + "/active.wal";
    std::string frozen_wal_path = db_path_ + "/frozen.wal";
    ::unlink(frozen_wal_path.c_str());
    if (::rename(active_wal_path.c_str(), frozen_wal_path.c_str()) != 0) {
        return Status::IOError("Failed to rename active WAL to frozen WAL");
    }

    // Create fresh active WAL and active memtable
    active_wal_ = new WALWriter(active_wal_path);
    frozen_memtable_ = active_memtable_;
    active_memtable_ = new SkipList();

    // Signal background thread
    {
        std::unique_lock<std::mutex> lock(bg_mutex_);
        flush_requested_ = true;
        bg_cv_.notify_all();
    }

    return Status::OK();
}

Status DB::WriteLevel0Table(SkipList* memtable, uint64_t file_num, FileMetaData* meta) {
    std::string sst_path = db_path_ + "/" + std::to_string(file_num) + ".sst";
    SSTableWriter writer(sst_path);

    std::string smallest = "";
    std::string largest = "";

    auto it = memtable->Begin();
    while (it.Valid()) {
        if (smallest.empty()) {
            smallest = it.key();
        }
        largest = it.key();
        
        Status s = writer.Append(it.key(), it.entry());
        if (!s.ok()) return s;
        it.Next();
    }

    Status s = writer.Finish();
    if (!s.ok()) return s;

    struct stat st;
    uint64_t size = 0;
    if (::stat(sst_path.c_str(), &st) == 0) {
        size = st.st_size;
    }

    *meta = FileMetaData{file_num, size, smallest, largest};
    return Status::OK();
}

Status DB::LoadSSTableReaders() {
    std::unique_lock<std::shared_mutex> lock(readers_mutex_);
    sstable_readers_.clear();

    for (int i = 0; i < Manifest::kMaxLevels; ++i) {
        for (const auto& meta : manifest_->GetLevelFiles(i)) {
            std::string path = db_path_ + "/" + std::to_string(meta.file_number) + ".sst";
            auto reader = std::make_shared<SSTableReader>(path);
            Status s = reader->Open();
            if (!s.ok()) {
                return s;
            }
            sstable_readers_[meta.file_number] = reader;
        }
    }
    return Status::OK();
}

Status DB::Recover() {
    std::string frozen_wal = db_path_ + "/frozen.wal";
    std::string active_wal = db_path_ + "/active.wal";

    // 1. Recover frozen WAL if it exists (indicates crash during flush)
    if (::access(frozen_wal.c_str(), F_OK) == 0) {
        SkipList temp_mem;
        WALReader reader(frozen_wal);
        Status s = reader.Recover(temp_mem);
        if (s.ok() && temp_mem.size() > 0) {
            uint64_t file_num = manifest_->NextFileNumber();
            FileMetaData meta;
            s = WriteLevel0Table(&temp_mem, file_num, &meta);
            if (s.ok()) {
                manifest_->AddFile(0, file_num, meta.file_size, meta.smallest_key, meta.largest_key);
                manifest_->Save();
            }
        }
        ::unlink(frozen_wal.c_str());
    }

    // 2. Recover active WAL if it exists
    if (::access(active_wal.c_str(), F_OK) == 0) {
        WALReader reader(active_wal);
        Status s = reader.Recover(*active_memtable_);
        if (!s.ok()) return s;
    }

    return Status::OK();
}

void DB::BackgroundWorker() {
    while (!shutting_down_) {
        std::unique_lock<std::mutex> lock(bg_mutex_);
        bg_cv_.wait(lock, [this]() { return shutting_down_ || flush_requested_; });

        if (shutting_down_) {
            break;
        }

        if (flush_requested_) {
            flush_requested_ = false;
            lock.unlock();

            // 1. Flush frozen memtable to Level 0
            SkipList* mem_to_flush = nullptr;
            {
                std::shared_lock<std::shared_mutex> db_lock(db_mutex_);
                mem_to_flush = frozen_memtable_;
            }

            if (mem_to_flush != nullptr) {
                uint64_t file_num = manifest_->NextFileNumber();
                FileMetaData meta;
                Status s = WriteLevel0Table(mem_to_flush, file_num, &meta);
                
                if (s.ok()) {
                    std::unique_lock<std::shared_mutex> db_lock(db_mutex_);
                    
                    std::string sst_path = db_path_ + "/" + std::to_string(file_num) + ".sst";
                    auto reader = std::make_shared<SSTableReader>(sst_path);
                    if (reader->Open().ok()) {
                        std::unique_lock<std::shared_mutex> readers_lock(readers_mutex_);
                        sstable_readers_[file_num] = reader;
                    }

                    manifest_->AddFile(0, file_num, meta.file_size, meta.smallest_key, meta.largest_key);
                    manifest_->Save();

                    delete frozen_memtable_;
                    frozen_memtable_ = nullptr;

                    ::unlink((db_path_ + "/frozen.wal").c_str());
                }
            }

            // 2. Check if compaction is needed
            bool needs_compaction = false;
            {
                std::shared_lock<std::shared_mutex> db_lock(db_mutex_);
                needs_compaction = manifest_->GetLevelFiles(0).size() >= l0_compaction_trigger_;
            }

            if (needs_compaction) {
                Status s = Compaction::Run(db_path_, manifest_.get());
                if (s.ok()) {
                    std::unique_lock<std::shared_mutex> db_lock(db_mutex_);
                    std::unique_lock<std::shared_mutex> readers_lock(readers_mutex_);

                    // Clean readers cache
                    std::map<uint64_t, bool> active_files;
                    for (int i = 0; i < Manifest::kMaxLevels; ++i) {
                        for (const auto& f : manifest_->GetLevelFiles(i)) {
                            active_files[f.file_number] = true;
                        }
                    }

                    // Remove obsolete reader entries
                    for (auto it = sstable_readers_.begin(); it != sstable_readers_.end();) {
                        if (active_files.find(it->first) == active_files.end()) {
                            it = sstable_readers_.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    // Load missing reader entries
                    for (auto it : active_files) {
                        if (sstable_readers_.find(it.first) == sstable_readers_.end()) {
                            std::string sst_path = db_path_ + "/" + std::to_string(it.first) + ".sst";
                            auto reader = std::make_shared<SSTableReader>(sst_path);
                            if (reader->Open().ok()) {
                                sstable_readers_[it.first] = reader;
                            }
                        }
                    }
                }
            }
        }
    }
}

std::string DB::GetStats() const {
    std::shared_lock<std::shared_mutex> db_lock(db_mutex_);
    std::stringstream ss;
    ss << "--- LSM DB Internals ---\n";
    ss << "MemTable Entries: " << active_memtable_->size() << "\n";
    ss << "MemTable Memory Estimate: " << active_memtable_->EstimateMemoryUsage() << " bytes\n";
    ss << manifest_->GetStats();
    return ss.str();
}

} // namespace lsm
