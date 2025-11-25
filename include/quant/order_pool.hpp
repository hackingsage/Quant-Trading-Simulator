#pragma once
#include <vector>
#include <cstdint>
#include <cassert>

namespace quant {

struct PoolOrder {
    uint64_t order_id;
    uint64_t user_id;
    uint8_t  side;       // 0 = buy, 1 = sell
    double   price;
    uint64_t quantity;
    uint64_t timestamp;

    uint32_t prev = UINT32_MAX;
    uint32_t next = UINT32_MAX;

    bool active = false;
};

class OrderPool {
public:
    explicit OrderPool(uint32_t capacity)
        : storage_(capacity), free_list_(capacity)
    {
        for (uint32_t i = 0; i < capacity; ++i)
            free_list_[i] = capacity - 1 - i;
    }

    uint32_t allocate() {
        assert(!free_list_.empty() && "OrderPool exhausted");
        uint32_t idx = free_list_.back();
        free_list_.pop_back();
        storage_[idx].active = true;
        storage_[idx].prev = storage_[idx].next = UINT32_MAX;
        return idx;
    }

    void release(uint32_t idx) {
        storage_[idx].active = false;
        storage_[idx].prev = storage_[idx].next = UINT32_MAX;
        free_list_.push_back(idx);
    }

    PoolOrder& operator[](uint32_t idx)             { return storage_[idx]; }
    const PoolOrder& operator[](uint32_t idx) const { return storage_[idx]; }

    bool is_active(uint32_t idx) const { return storage_[idx].active; }

private:
    std::vector<PoolOrder> storage_;
    std::vector<uint32_t>  free_list_;
};

} // namespace quant
