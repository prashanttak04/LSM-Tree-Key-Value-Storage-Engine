#include "wal.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace lsm {

static uint32_t FNV1a(const char* data, size_t size) {
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 16777619U;
    }
    return hash;
}

WALWriter::WALWriter(const std::string& filepath) : fd_(-1), filepath_(filepath) {
    // Open in write-only, create if not exist, append mode
    fd_ = ::open(filepath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

WALWriter::~WALWriter() {
    Close();
}

Status WALWriter::Append(const std::string& key, const std::string& value, ValueType type) {
    if (fd_ < 0) {
        return Status::IOError("WAL file not open for writing: " + filepath_);
    }

    // Build the record payload buffer
    std::string payload;
    payload.push_back(static_cast<char>(type));
    PutFixed32(&payload, static_cast<uint32_t>(key.size()));
    payload.append(key);
    PutFixed32(&payload, static_cast<uint32_t>(value.size()));
    payload.append(value);

    // Compute checksum
    uint32_t checksum = FNV1a(payload.data(), payload.size());

    // Assemble final record
    std::string record;
    PutFixed32(&record, checksum);
    record.append(payload);

    // Write to disk
    ssize_t written = ::write(fd_, record.data(), record.size());
    if (written < 0 || static_cast<size_t>(written) != record.size()) {
        return Status::IOError("Failed to write to WAL file: " + filepath_);
    }

    return Status::OK();
}

Status WALWriter::Sync() {
    if (fd_ < 0) {
        return Status::IOError("WAL file not open: " + filepath_);
    }
    if (::fsync(fd_) < 0) {
        return Status::IOError("fsync failed on WAL: " + filepath_);
    }
    return Status::OK();
}

void WALWriter::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

WALReader::WALReader(const std::string& filepath) : fd_(-1), filepath_(filepath) {
    fd_ = ::open(filepath.c_str(), O_RDONLY);
}

WALReader::~WALReader() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Status WALReader::Recover(SkipList& memtable) {
    if (fd_ < 0) {
        // If file doesn't exist, that's fine (first run), just return OK
        if (errno == ENOENT) {
            return Status::OK();
        }
        return Status::IOError("Failed to open WAL file for reading: " + filepath_);
    }

    // Read loop
    while (true) {
        uint32_t logged_checksum = 0;
        ssize_t read_bytes = ::read(fd_, &logged_checksum, sizeof(uint32_t));
        if (read_bytes == 0) {
            // EOF reached cleanly
            break;
        }
        if (read_bytes < 0) {
            return Status::IOError("Error reading WAL file: " + filepath_);
        }
        if (static_cast<size_t>(read_bytes) != sizeof(uint32_t)) {
            // Truncated/corrupted file at the end
            return Status::Corruption("WAL truncated: partial checksum read");
        }

        // Read Type (1 byte) + Key Length (4 bytes)
        uint8_t type_val = 0;
        uint32_t key_len = 0;

        if (::read(fd_, &type_val, sizeof(uint8_t)) != sizeof(uint8_t)) {
            return Status::Corruption("WAL record corrupted: failed to read type");
        }
        if (::read(fd_, &key_len, sizeof(uint32_t)) != sizeof(uint32_t)) {
            return Status::Corruption("WAL record corrupted: failed to read key length");
        }

        // Read Key
        std::vector<char> key_buf(key_len);
        if (key_len > 0) {
            if (::read(fd_, key_buf.data(), key_len) != static_cast<ssize_t>(key_len)) {
                return Status::Corruption("WAL record corrupted: failed to read key");
            }
        }
        std::string key(key_buf.data(), key_len);

        // Read Value Length
        uint32_t val_len = 0;
        if (::read(fd_, &val_len, sizeof(uint32_t)) != sizeof(uint32_t)) {
            return Status::Corruption("WAL record corrupted: failed to read value length");
        }

        // Read Value
        std::vector<char> val_buf(val_len);
        if (val_len > 0) {
            if (::read(fd_, val_buf.data(), val_len) != static_cast<ssize_t>(val_len)) {
                return Status::Corruption("WAL record corrupted: failed to read value");
            }
        }
        std::string value(val_buf.data(), val_len);

        // Reconstruct payload to verify checksum
        std::string payload;
        payload.push_back(static_cast<char>(type_val));
        PutFixed32(&payload, key_len);
        payload.append(key);
        PutFixed32(&payload, val_len);
        payload.append(value);

        if (FNV1a(payload.data(), payload.size()) != logged_checksum) {
            return Status::Corruption("WAL record checksum mismatch in file: " + filepath_);
        }

        // Insert into MemTable
        memtable.Insert(key, Entry{value, static_cast<ValueType>(type_val)});
    }

    return Status::OK();
}

} // namespace lsm
