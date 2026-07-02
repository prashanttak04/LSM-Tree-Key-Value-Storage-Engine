#ifndef LSM_BLOOM_H
#define LSM_BLOOM_H

#include "common.h"
#include <vector>
#include <cmath>

namespace lsm {

static uint32_t FNV1aSeeded(const char* data, size_t size, uint32_t seed) {
    uint32_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 16777619U;
    }
    return hash;
}

class BloomFilter {
public:
    // Create filter with default bit size or calculated optimal sizing
    BloomFilter(size_t num_keys, double false_positive_rate = 0.02) {
        if (num_keys == 0) num_keys = 1;
        // Formula: m = - (n * ln(p)) / (ln(2)^2)
        double m_bits = - (static_cast<double>(num_keys) * std::log(false_positive_rate)) / (std::log(2) * std::log(2));
        size_t num_bits = static_cast<size_t>(std::ceil(m_bits));
        size_t num_bytes = (num_bits + 7) / 8;
        if (num_bytes == 0) num_bytes = 1;
        
        bits_.assign(num_bytes, 0);
        bit_size_ = num_bytes * 8;
        
        // Formula: k = (m/n) * ln(2)
        double k_hashes = (static_cast<double>(bit_size_) / num_keys) * std::log(2);
        num_hashes_ = static_cast<size_t>(std::round(k_hashes));
        if (num_hashes_ == 0) num_hashes_ = 1;
    }

    // Constructor to load serialized filter data
    BloomFilter(const char* data, size_t size, size_t num_hashes) 
        : bits_(data, data + size), bit_size_(size * 8), num_hashes_(num_hashes) {}

    void Add(const std::string& key) {
        uint32_t h1 = FNV1aSeeded(key.data(), key.size(), 2166136261U);
        uint32_t h2 = FNV1aSeeded(key.data(), key.size(), 1337U);

        for (size_t i = 0; i < num_hashes_; ++i) {
            uint32_t bit_idx = (h1 + i * h2) % bit_size_;
            bits_[bit_idx / 8] |= (1 << (bit_idx % 8));
        }
    }

    bool Contains(const std::string& key) const {
        if (bits_.empty()) return false;
        
        uint32_t h1 = FNV1aSeeded(key.data(), key.size(), 2166136261U);
        uint32_t h2 = FNV1aSeeded(key.data(), key.size(), 1337U);

        for (size_t i = 0; i < num_hashes_; ++i) {
            uint32_t bit_idx = (h1 + i * h2) % bit_size_;
            if (!(bits_[bit_idx / 8] & (1 << (bit_idx % 8)))) {
                return false; // Definitely not in set
            }
        }
        return true; // Might be in set
    }

    const std::vector<uint8_t>& GetData() const {
        return bits_;
    }

    size_t GetNumHashes() const {
        return num_hashes_;
    }

private:
    std::vector<uint8_t> bits_;
    size_t bit_size_;
    size_t num_hashes_;
};

} // namespace lsm

#endif // LSM_BLOOM_H
