#ifndef LSM_COMMON_H
#define LSM_COMMON_H

#include <string>
#include <vector>
#include <iostream>
#include <cstdint>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <shared_mutex>
#include <thread>
#include <chrono>
#include <sstream>
#include <fstream>
#include <cmath>

namespace lsm {

// Operation Types
enum ValueType : uint8_t {
    kTypeValue = 0,
    kTypeDeletion = 1
};

struct Entry {
    std::string value;
    ValueType type;
};

// Return codes for DB operations
enum class StatusCode {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4
};

class Status {
public:
    Status() : code_(StatusCode::kOk), msg_("") {}
    Status(StatusCode code, const std::string& msg) : code_(code), msg_(msg) {}

    static Status OK() { return Status(StatusCode::kOk, ""); }
    static Status NotFound(const std::string& msg) { return Status(StatusCode::kNotFound, msg); }
    static Status Corruption(const std::string& msg) { return Status(StatusCode::kCorruption, msg); }
    static Status IOError(const std::string& msg) { return Status(StatusCode::kIOError, msg); }
    static Status InvalidArgument(const std::string& msg) { return Status(StatusCode::kInvalidArgument, msg); }

    bool ok() const { return code_ == StatusCode::kOk; }
    bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
    bool IsCorruption() const { return code_ == StatusCode::kCorruption; }
    bool IsIOError() const { return code_ == StatusCode::kIOError; }
    
    std::string ToString() const {
        if (ok()) return "OK";
        std::string res;
        switch (code_) {
            case StatusCode::kNotFound: res = "NotFound: "; break;
            case StatusCode::kCorruption: res = "Corruption: "; break;
            case StatusCode::kIOError: res = "IOError: "; break;
            case StatusCode::kInvalidArgument: res = "InvalidArgument: "; break;
            default: res = "Unknown: "; break;
        }
        res.append(msg_);
        return res;
    }

private:
    StatusCode code_;
    std::string msg_;
};

// Serialization Helpers
inline void PutFixed32(std::string* dst, uint32_t value) {
    char buf[sizeof(value)];
    std::memcpy(buf, &value, sizeof(value));
    dst->append(buf, sizeof(value));
}

inline void PutFixed64(std::string* dst, uint64_t value) {
    char buf[sizeof(value)];
    std::memcpy(buf, &value, sizeof(value));
    dst->append(buf, sizeof(value));
}

inline void PutLengthPrefixedSlice(std::string* dst, const std::string& value) {
    PutFixed32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

inline bool GetFixed32(const char** src, const char* limit, uint32_t* value) {
    if (*src + sizeof(uint32_t) > limit) {
        return false;
    }
    std::memcpy(value, *src, sizeof(uint32_t));
    *src += sizeof(uint32_t);
    return true;
}

inline bool GetFixed64(const char** src, const char* limit, uint64_t* value) {
    if (*src + sizeof(uint64_t) > limit) {
        return false;
    }
    std::memcpy(value, *src, sizeof(uint64_t));
    *src += sizeof(uint64_t);
    return true;
}

inline bool GetByte(const char** src, const char* limit, uint8_t* value) {
    if (*src + sizeof(uint8_t) > limit) {
        return false;
    }
    *value = static_cast<uint8_t>(**src);
    *src += sizeof(uint8_t);
    return true;
}

inline bool GetLengthPrefixedSlice(const char** src, const char* limit, std::string* value) {
    uint32_t len;
    if (!GetFixed32(src, limit, &len)) {
        return false;
    }
    if (*src + len > limit) {
        return false;
    }
    value->assign(*src, len);
    *src += len;
    return true;
}

} // namespace lsm

#endif // LSM_COMMON_H
