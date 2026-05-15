#pragma once

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace ob {

template <class T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : storage_(capacity) {
        for (std::size_t i = 0; i + 1 < capacity; ++i) {
            storage_[i].next_free = &storage_[i + 1];
        }
        storage_.back().next_free = nullptr;
        free_head_ = &storage_.front();
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <class... Args>
    [[nodiscard]] T* acquire(Args&&... args) noexcept {
        if (!free_head_) return nullptr;
        Slot* s = free_head_;
        free_head_ = s->next_free;
        return ::new (s->storage) T(std::forward<Args>(args)...);
    }

    void release(T* p) noexcept {
        if (!p) return;
        p->~T();
        auto* s = reinterpret_cast<Slot*>(p);
        s->next_free = free_head_;
        free_head_ = s;
    }

    std::size_t capacity() const noexcept { return storage_.size(); }

private:
    union Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        Slot* next_free;
        Slot() : next_free(nullptr) {}
        ~Slot() {}
    };

    std::vector<Slot> storage_;
    Slot* free_head_{nullptr};
};

}  // namespace ob
