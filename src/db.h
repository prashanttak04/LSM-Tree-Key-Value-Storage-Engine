#ifndef LSM_DB_H
#define LSM_DB_H

#include "common.h"
#include "skiplist.h"
#include "wal.h"
#include "manifest.h"
#include "sstable.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include <map>

namespace lsm {

class DB {
public:
    static Status Open(const std::string& db_path, DB** dbptr);
    ~DB();

    // Disable copy
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    Status Put(const std::string& key, const std::string& value);
    Status Get(const std::string& key, std::string* value);
    Status Delete(const std::string& key);
    Status Scan(const std::string& start_key, const std::string& end_key, 
                std::vector<std::pair<std::string, std::string>>& results);

    std::string GetStats() const;

private:
    explicit DB(const std::string& db_path);

    // Load active SSTable readers into memory
    Status LoadSSTableReaders();
    
    // Recovery routine on startup
    Status Recover();

    // Helper to flush a memtable to a Level 0 SSTable file
    Status WriteLevel0Table(SkipList* memtable, uint64_t file_num, FileMetaData* meta);

    // Background worker loop for flushing and compaction
    void BackgroundWorker();

    // Triggered when active MemTable is full
    Status MaybeScheduleFlush();

    std::string db_path_;
    
    // Active and frozen memtables
    SkipList* active_memtable_;
    SkipList* frozen_memtable_;
    
    // Active WAL writer
    WALWriter* active_wal_;

    // Manifest catalog
    std::unique_ptr<Manifest> manifest_;

    // In-memory cache of SSTable readers: [file_number -> reader]
    // Protected by readers_mutex_
    mutable std::shared_mutex readers_mutex_;
    std::map<uint64_t, std::shared_ptr<SSTableReader>> sstable_readers_;

    // Concurrency control for DB operations (writes/flushes)
    mutable std::shared_mutex db_mutex_;

    // Background thread state
    std::thread bg_thread_;
    std::atomic<bool> shutting_down_;
    std::mutex bg_mutex_;
    std::condition_variable bg_cv_;
    std::atomic<bool> flush_requested_;

    // Settings
    const size_t max_memtable_size_; // e.g. 2MB trigger
    const size_t l0_compaction_trigger_; // L0 file count to trigger compaction
};

} // namespace lsm

#endif // LSM_DB_H
