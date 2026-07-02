#ifndef LSM_SKIPLIST_H
#define LSM_SKIPLIST_H

#include "common.h"
#include <random>
#include <shared_mutex>
#include <vector>

namespace lsm {

class SkipList {
private:
    struct Node {
        std::string key;
        Entry entry;
        // Array of pointers to next nodes at different levels
        std::vector<Node*> forward;

        Node(const std::string& k, const Entry& e, int height)
            : key(k), entry(e), forward(height, nullptr) {}
    };

public:
    explicit SkipList(int max_height = 12, float probability = 0.25f)
        : max_height_(max_height),
          probability_(probability),
          active_height_(1),
          size_(0),
          estimated_memory_(0) {
        
        // Dummy head node with default constructed key/entry
        head_ = new Node("", Entry{"", kTypeValue}, max_height_);
        
        // Seed random number generator
        std::random_device rd;
        rng_.seed(rd());
        dis_ = std::uniform_real_distribution<float>(0.0f, 1.0f);
    }

    ~SkipList() {
        Clear();
        delete head_;
    }

    // Disable copy constructor and assignment operator
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    void Insert(const std::string& key, const Entry& entry) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        std::vector<Node*> update(max_height_, nullptr);
        Node* curr = head_;

        // Traverse down from active height to level 0
        for (int i = active_height_ - 1; i >= 0; i--) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
                curr = curr->forward[i];
            }
            update[i] = curr;
        }

        curr = (curr->forward.empty()) ? nullptr : curr->forward[0];

        // If key already exists, update its value and adjust memory estimate
        if (curr != nullptr && curr->key == key) {
            estimated_memory_ -= curr->entry.value.size();
            curr->entry = entry;
            estimated_memory_ += entry.value.size();
            return;
        }

        // Key doesn't exist, create a new node
        int new_height = RandomHeightInternal();
        if (new_height > active_height_) {
            for (int i = active_height_; i < new_height; i++) {
                update[i] = head_;
            }
            active_height_ = new_height;
        }

        Node* new_node = new Node(key, entry, new_height);
        for (int i = 0; i < new_height; i++) {
            new_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = new_node;
        }

        size_++;
        // Estimate memory usage: key size, value size, Node struct, and vector pointer overhead
        estimated_memory_ += key.size() + entry.value.size() + sizeof(Node) + (new_height * sizeof(Node*));
    }

    bool Find(const std::string& key, Entry* entry) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        Node* curr = head_;

        for (int i = active_height_ - 1; i >= 0; i--) {
            while (curr->forward[i] != nullptr && curr->forward[i]->key < key) {
                curr = curr->forward[i];
            }
        }

        curr = curr->forward[0];
        if (curr != nullptr && curr->key == key) {
            *entry = curr->entry;
            return true;
        }
        return false;
    }

    bool Contains(const std::string& key) const {
        Entry ent;
        return Find(key, &ent);
    }

    void Clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        Node* curr = head_->forward[0];
        while (curr != nullptr) {
            Node* next = curr->forward[0];
            delete curr;
            curr = next;
        }
        for (int i = 0; i < max_height_; i++) {
            head_->forward[i] = nullptr;
        }
        active_height_ = 1;
        size_ = 0;
        estimated_memory_ = 0;
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return size_;
    }

    size_t EstimateMemoryUsage() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return estimated_memory_;
    }

    // Forward Iterator for scanning the database or flushing to disk.
    // Note: The iterator assumes the SkipList is read-only (e.g. during flush/scan lock).
    class Iterator {
    public:
        explicit Iterator(Node* node) : current_(node) {}
        bool Valid() const { return current_ != nullptr; }
        void Next() { current_ = current_->forward[0]; }
        const std::string& key() const { return current_->key; }
        const Entry& entry() const { return current_->entry; }
    private:
        Node* current_;
    };

    Iterator Begin() const {
        return Iterator(head_->forward[0]);
    }

private:
    int RandomHeightInternal() {
        int height = 1;
        while (height < max_height_ && dis_(rng_) < probability_) {
            height++;
        }
        return height;
    }

    const int max_height_;
    const float probability_;
    int active_height_;
    size_t size_;
    size_t estimated_memory_;
    Node* head_;
    
    // Concurrency control
    mutable std::shared_mutex mutex_;

    // RNG for height generation
    std::mt19937 rng_;
    std::uniform_real_distribution<float> dis_;
};

} // namespace lsm

#endif // LSM_SKIPLIST_H
