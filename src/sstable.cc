#include "sstable.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>

namespace lsm {

SSTableWriter::SSTableWriter(const std::string& filepath)
    : filepath_(filepath), offset_(0), record_count_(0), sparse_index_interval_(16) {
    file_.open(filepath, std::ios::binary | std::ios::out);
}

SSTableWriter::~SSTableWriter() {
    if (file_.is_open()) {
        file_.close();
    }
}

Status SSTableWriter::Append(const std::string& key, const Entry& entry) {
    if (!file_.is_open()) {
        return Status::IOError("SSTable file not open: " + filepath_);
    }

    // Build sparse index
    if (record_count_ % sparse_index_interval_ == 0) {
        index_.push_back({key, offset_});
    }

    // Record structure: [key_len: uint32_t] [val_len: uint32_t] [type: uint8_t] [key] [value]
    std::string buf;
    PutFixed32(&buf, static_cast<uint32_t>(key.size()));
    PutFixed32(&buf, static_cast<uint32_t>(entry.value.size()));
    buf.push_back(static_cast<char>(entry.type));
    buf.append(key);
    buf.append(entry.value);

    file_.write(buf.data(), buf.size());
    if (file_.fail()) {
        return Status::IOError("Failed to write to SSTable file: " + filepath_);
    }

    offset_ += buf.size();
    keys_.push_back(key);
    record_count_++;

    return Status::OK();
}

Status SSTableWriter::Finish() {
    if (!file_.is_open()) {
        return Status::IOError("SSTable file not open: " + filepath_);
    }

    // 1. Write Index Block
    uint64_t index_offset = offset_;
    std::string index_buf;
    for (const auto& idx_entry : index_) {
        // Index entry format: [key_len: uint32_t] [offset: uint64_t] [key]
        PutFixed32(&index_buf, static_cast<uint32_t>(idx_entry.first.size()));
        PutFixed64(&index_buf, idx_entry.second);
        index_buf.append(idx_entry.first);
    }
    file_.write(index_buf.data(), index_buf.size());
    if (file_.fail()) {
        return Status::IOError("Failed to write SSTable index: " + filepath_);
    }
    uint64_t index_size = index_buf.size();
    offset_ += index_size;

    // 2. Write Bloom Filter Block
    uint64_t bloom_offset = offset_;
    BloomFilter bloom(keys_.size() == 0 ? 1 : keys_.size(), 0.02);
    for (const auto& key : keys_) {
        bloom.Add(key);
    }

    const auto& filter_data = bloom.GetData();
    std::string bloom_buf;
    PutFixed32(&bloom_buf, static_cast<uint32_t>(bloom.GetNumHashes()));
    PutFixed32(&bloom_buf, static_cast<uint32_t>(filter_data.size()));
    bloom_buf.append(reinterpret_cast<const char*>(filter_data.data()), filter_data.size());

    file_.write(bloom_buf.data(), bloom_buf.size());
    if (file_.fail()) {
        return Status::IOError("Failed to write SSTable Bloom filter: " + filepath_);
    }
    uint64_t bloom_size = bloom_buf.size();
    offset_ += bloom_size;

    // 3. Write Footer (Fixed 40 bytes)
    std::string footer_buf;
    PutFixed64(&footer_buf, index_offset);
    PutFixed64(&footer_buf, index_size);
    PutFixed64(&footer_buf, bloom_offset);
    PutFixed64(&footer_buf, bloom_size);
    PutFixed64(&footer_buf, 0x1577771B); // Magic number

    file_.write(footer_buf.data(), footer_buf.size());
    if (file_.fail()) {
        return Status::IOError("Failed to write SSTable footer: " + filepath_);
    }

    file_.close();
    return Status::OK();
}


SSTableReader::SSTableReader(const std::string& filepath)
    : filepath_(filepath), fd_(-1), file_size_(0) {}

SSTableReader::~SSTableReader() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Status SSTableReader::Open() {
    fd_ = ::open(filepath_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        return Status::IOError("Failed to open SSTable for reading: " + filepath_);
    }

    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        return Status::IOError("Failed to fstat SSTable file: " + filepath_);
    }
    file_size_ = st.st_size;

    if (file_size_ < 40) {
        return Status::Corruption("SSTable file size too small to be valid: " + filepath_);
    }

    // 1. Read Footer (40 bytes)
    char footer_bytes[40];
    if (::lseek(fd_, file_size_ - 40, SEEK_SET) < 0) {
        return Status::IOError("Failed to seek to SSTable footer: " + filepath_);
    }
    if (::read(fd_, footer_bytes, 40) != 40) {
        return Status::IOError("Failed to read SSTable footer: " + filepath_);
    }

    const char* ptr = footer_bytes;
    const char* limit = footer_bytes + 40;
    uint64_t index_offset = 0;
    uint64_t index_size = 0;
    uint64_t bloom_offset = 0;
    uint64_t bloom_size = 0;
    uint64_t magic = 0;

    GetFixed64(&ptr, limit, &index_offset);
    GetFixed64(&ptr, limit, &index_size);
    GetFixed64(&ptr, limit, &bloom_offset);
    GetFixed64(&ptr, limit, &bloom_size);
    GetFixed64(&ptr, limit, &magic);

    if (magic != 0x1577771B) {
        return Status::Corruption("SSTable magic number mismatch: invalid file structure");
    }

    // 2. Load Bloom Filter Block
    if (::lseek(fd_, bloom_offset, SEEK_SET) < 0) {
        return Status::IOError("Failed to seek to Bloom Filter: " + filepath_);
    }
    uint32_t num_hashes = 0;
    uint32_t filter_len = 0;
    if (::read(fd_, &num_hashes, sizeof(uint32_t)) != sizeof(uint32_t) ||
        ::read(fd_, &filter_len, sizeof(uint32_t)) != sizeof(uint32_t)) {
        return Status::IOError("Failed to read Bloom Filter headers: " + filepath_);
    }

    std::vector<char> filter_buf(filter_len);
    if (::read(fd_, filter_buf.data(), filter_len) != static_cast<ssize_t>(filter_len)) {
        return Status::IOError("Failed to read Bloom Filter bit array: " + filepath_);
    }
    bloom_ = std::make_unique<BloomFilter>(filter_buf.data(), filter_len, num_hashes);

    // 3. Load Index Block
    if (::lseek(fd_, index_offset, SEEK_SET) < 0) {
        return Status::IOError("Failed to seek to Index Block: " + filepath_);
    }
    std::vector<char> index_buf(index_size);
    if (::read(fd_, index_buf.data(), index_size) != static_cast<ssize_t>(index_size)) {
        return Status::IOError("Failed to read Index Block: " + filepath_);
    }

    const char* idx_ptr = index_buf.data();
    const char* idx_limit = index_buf.data() + index_size;
    while (idx_ptr < idx_limit) {
        uint32_t key_len = 0;
        uint64_t offset = 0;
        if (!GetFixed32(&idx_ptr, idx_limit, &key_len) ||
            !GetFixed64(&idx_ptr, idx_limit, &offset)) {
            return Status::Corruption("SSTable index block corrupted: parse error");
        }
        if (idx_ptr + key_len > idx_limit) {
            return Status::Corruption("SSTable index block corrupted: key length out of bounds");
        }
        std::string key(idx_ptr, key_len);
        idx_ptr += key_len;
        index_.push_back({key, offset});
    }

    return Status::OK();
}

bool SSTableReader::Get(const std::string& key, Entry* entry) {
    // Check Bloom filter
    if (!bloom_ || !bloom_->Contains(key)) {
        return false;
    }

    if (index_.empty()) {
        return false;
    }

    // Binary search index to find candidate block
    // We want the last index record where record.key <= key.
    auto it = std::upper_bound(index_.begin(), index_.end(), key, 
        [](const std::string& k, const std::pair<std::string, uint64_t>& element) {
            return k < element.first;
        });

    if (it == index_.begin()) {
        // key is smaller than the first key in the file
        if (key < index_[0].first) {
            return false;
        }
    } else {
        --it;
    }

    uint64_t start_offset = it->second;
    uint64_t end_offset = 0;
    
    // Find scan boundary: next index entry or start of index block
    auto next_it = it + 1;
    if (next_it == index_.end()) {
        // Read footer to find where data block ends (which is the index_offset)
        // We'll just read from file size - 40 to get index_offset
        char footer_bytes[8];
        if (::lseek(fd_, file_size_ - 40, SEEK_SET) >= 0 &&
            ::read(fd_, footer_bytes, 8) == 8) {
            std::memcpy(&end_offset, footer_bytes, 8);
        } else {
            end_offset = file_size_; // fallback
        }
    } else {
        end_offset = next_it->second;
    }

    // Seek and scan sequentially
    if (::lseek(fd_, start_offset, SEEK_SET) < 0) {
        return false;
    }

    uint64_t curr = start_offset;
    while (curr < end_offset) {
        uint32_t k_len = 0;
        uint32_t v_len = 0;
        if (::read(fd_, &k_len, sizeof(uint32_t)) != sizeof(uint32_t) ||
            ::read(fd_, &v_len, sizeof(uint32_t)) != sizeof(uint32_t)) {
            return false;
        }

        uint8_t type_val = 0;
        if (::read(fd_, &type_val, sizeof(uint8_t)) != sizeof(uint8_t)) {
            return false;
        }

        std::vector<char> k_buf(k_len);
        if (::read(fd_, k_buf.data(), k_len) != static_cast<ssize_t>(k_len)) {
            return false;
        }
        std::string record_key(k_buf.data(), k_len);

        std::vector<char> v_buf(v_len);
        if (::read(fd_, v_buf.data(), v_len) != static_cast<ssize_t>(v_len)) {
            return false;
        }
        std::string record_val(v_buf.data(), v_len);

        curr += sizeof(uint32_t) * 2 + sizeof(uint8_t) + k_len + v_len;

        if (record_key == key) {
            *entry = Entry{record_val, static_cast<ValueType>(type_val)};
            return true;
        } else if (record_key > key) {
            // Since file keys are sorted, we can stop early
            return false;
        }
    }

    return false;
}

void SSTableReader::Scan(const std::string& start_key, const std::string& end_key, 
                         std::vector<std::pair<std::string, Entry>>& results) {
    if (index_.empty()) return;

    // Find starting block offset
    auto it = std::upper_bound(index_.begin(), index_.end(), start_key, 
        [](const std::string& k, const std::pair<std::string, uint64_t>& element) {
            return k < element.first;
        });

    if (it != index_.begin()) {
        --it;
    }

    uint64_t start_offset = it->second;
    uint64_t data_end_offset = 0;

    // Load start of index block as data_end_offset
    char footer_bytes[8];
    if (::lseek(fd_, file_size_ - 40, SEEK_SET) >= 0 &&
        ::read(fd_, footer_bytes, 8) == 8) {
        std::memcpy(&data_end_offset, footer_bytes, 8);
    } else {
        data_end_offset = file_size_; // fallback
    }

    if (::lseek(fd_, start_offset, SEEK_SET) < 0) {
        return;
    }

    uint64_t curr = start_offset;
    while (curr < data_end_offset) {
        uint32_t k_len = 0;
        uint32_t v_len = 0;
        if (::read(fd_, &k_len, sizeof(uint32_t)) != sizeof(uint32_t) ||
            ::read(fd_, &v_len, sizeof(uint32_t)) != sizeof(uint32_t)) {
            break;
        }

        uint8_t type_val = 0;
        if (::read(fd_, &type_val, sizeof(uint8_t)) != sizeof(uint8_t)) {
            break;
        }

        std::vector<char> k_buf(k_len);
        if (::read(fd_, k_buf.data(), k_len) != static_cast<ssize_t>(k_len)) {
            break;
        }
        std::string record_key(k_buf.data(), k_len);

        std::vector<char> v_buf(v_len);
        if (::read(fd_, v_buf.data(), v_len) != static_cast<ssize_t>(v_len)) {
            break;
        }
        std::string record_val(v_buf.data(), v_len);

        curr += sizeof(uint32_t) * 2 + sizeof(uint8_t) + k_len + v_len;

        if (record_key > end_key) {
            break; // Out of range, keys are sorted
        }

        if (record_key >= start_key) {
            results.push_back({record_key, Entry{record_val, static_cast<ValueType>(type_val)}});
        }
    }
}

SSTableIterator::SSTableIterator(const std::string& filepath)
    : fd_(-1), curr_offset_(0), data_end_offset_(0), valid_(false) {
    fd_ = ::open(filepath.c_str(), O_RDONLY);
    if (fd_ < 0) return;

    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    uint64_t file_size = st.st_size;
    if (file_size < 40) {
        ::close(fd_);
        fd_ = -1;
        return;
    }

    // Read footer to find index_offset (end of data block)
    char footer_bytes[8];
    if (::lseek(fd_, file_size - 40, SEEK_SET) >= 0 && ::read(fd_, footer_bytes, 8) == 8) {
        std::memcpy(&data_end_offset_, footer_bytes, 8);
    } else {
        data_end_offset_ = file_size;
    }

    // Seek back to start of data block (offset 0)
    ::lseek(fd_, 0, SEEK_SET);

    valid_ = true;
    Next(); // Load the first record
}

SSTableIterator::~SSTableIterator() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void SSTableIterator::Next() {
    if (!valid_ || fd_ < 0) return;

    if (curr_offset_ >= data_end_offset_) {
        valid_ = false;
        return;
    }

    uint32_t k_len = 0;
    uint32_t v_len = 0;
    if (::read(fd_, &k_len, sizeof(uint32_t)) != sizeof(uint32_t) ||
        ::read(fd_, &v_len, sizeof(uint32_t)) != sizeof(uint32_t)) {
        valid_ = false;
        return;
    }

    uint8_t type_val = 0;
    if (::read(fd_, &type_val, sizeof(uint8_t)) != sizeof(uint8_t)) {
        valid_ = false;
        return;
    }

    std::vector<char> k_buf(k_len);
    if (::read(fd_, k_buf.data(), k_len) != static_cast<ssize_t>(k_len)) {
        valid_ = false;
        return;
    }
    curr_key_ = std::string(k_buf.data(), k_len);

    std::vector<char> v_buf(v_len);
    if (::read(fd_, v_buf.data(), v_len) != static_cast<ssize_t>(v_len)) {
        valid_ = false;
        return;
    }
    curr_entry_ = Entry{std::string(v_buf.data(), v_len), static_cast<ValueType>(type_val)};

    curr_offset_ += sizeof(uint32_t) * 2 + sizeof(uint8_t) + k_len + v_len;
}

} // namespace lsm
