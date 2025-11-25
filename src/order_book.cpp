#include "quant/order_book.hpp"
#include <algorithm>

namespace quant {

// Reserve large pool capacity to avoid dynamic allocations in matching hot path.
OrderBook::OrderBook(const std::string& symbol)
    : symbol_(symbol),
      pool_(1'000'000'000) // capacity
{}

// ID generators
uint64_t OrderBook::allocate_order_id()    { return next_order_id_++; }
uint64_t OrderBook::allocate_trade_id()    { return next_trade_id_++; }
uint64_t OrderBook::allocate_timestamp()   { return next_timestamp_++; }

// Helpers
bool OrderBook::level_empty(const PriceLevel& level) const {
    return level.head == UINT32_MAX;
}

// Append node at tail; maintain FIFO within price level.
void OrderBook::append_to_level(PriceLevel& level, uint32_t idx) {
    PoolOrder& po = pool_[idx];
    po.prev = level.tail;
    po.next = UINT32_MAX;
    if (level.tail != UINT32_MAX)
        pool_[level.tail].next = idx;
    level.tail = idx;
    if (level.head == UINT32_MAX)
        level.head = idx;
}

// Unlink node from a price level; update head/tail boundaries.
void OrderBook::unlink_from_level(PriceLevel& level, uint32_t idx) {
    PoolOrder& po = pool_[idx];
    if (po.prev != UINT32_MAX)
        pool_[po.prev].next = po.next;
    if (po.next != UINT32_MAX)
        pool_[po.next].prev = po.prev;
    if (level.head == idx)
        level.head = po.next;
    if (level.tail == idx)
        level.tail = po.prev;
    po.prev = po.next = UINT32_MAX;
}

// ---------------- Public API ----------------

uint64_t OrderBook::submit_limit_order(const Order& order,
                                       std::vector<Trade>& out_trades)
{
    out_trades.clear();
    // Fast-path: ignore zero-quantity orders.
    if (order.quantity == 0) return 0;

    Order incoming = order;
    if (incoming.order_id == 0)
        incoming.order_id = allocate_order_id();
    if (incoming.ts_ns == 0)
        incoming.ts_ns = allocate_timestamp();

    if (incoming.side == Side::Buy) {
        match_buy(incoming, out_trades);
        if (incoming.quantity > 0) {
            add_to_book(incoming);
            return incoming.order_id;
        }
        return 0;
    } else {
        match_sell(incoming, out_trades);
        if (incoming.quantity > 0) {
            add_to_book(incoming);
            return incoming.order_id;
        }
        return 0;
    }
}

// Cancel by order_id: locate side/price via index, unlink from level, release pool node.
bool OrderBook::cancel_order(uint64_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;
    OrderRef ref = it->second;
    order_index_.erase(it);

    if (ref.side == Side::Buy) {
        auto lvl_it = bids_.find(ref.price);
        if (lvl_it == bids_.end()) return false;
        PriceLevel& level = lvl_it->second;
        unlink_from_level(level, ref.idx);
        pool_.release(ref.idx);
        if (level_empty(level)) bids_.erase(lvl_it);
    } else {
        auto lvl_it = asks_.find(ref.price);
        if (lvl_it == asks_.end()) return false;
        PriceLevel& level = lvl_it->second;
        unlink_from_level(level, ref.idx);
        pool_.release(ref.idx);
        if (level_empty(level)) asks_.erase(lvl_it);
    }

    return true;
}

// ---------------- Top of Book ----------------

// Aggregate best bid/ask price levels into a compact TOB snapshot.
TopOfBook OrderBook::top_of_book() const {
    TopOfBook tob;

    if (!bids_.empty()) {
        const auto& best = *bids_.begin();
        tob.has_bid = true;
        tob.bid_price = best.first;
        uint64_t sum = 0;
        uint32_t idx = best.second.head;
        while (idx != UINT32_MAX) {
            const PoolOrder& po = pool_[idx];
            sum += po.quantity;
            idx = po.next;
        }
        tob.bid_quantity = sum;
    }

    if (!asks_.empty()) {
        const auto& best = *asks_.begin();
        tob.has_ask = true;
        tob.ask_price = best.first;
        uint64_t sum = 0;
        uint32_t idx = best.second.head;
        while (idx != UINT32_MAX) {
            const PoolOrder& po = pool_[idx];
            sum += po.quantity;
            idx = po.next;
        }
        tob.ask_quantity = sum;
    }

    return tob;
}

// ---------------- Snapshots (L2 levels) ----------------

std::vector<std::pair<double,uint64_t>> OrderBook::snapshot_bids() const {
    std::vector<std::pair<double,uint64_t>> out;
    out.reserve(bids_.size());
    for (const auto& kv : bids_) {
        double price = kv.first;
        const PriceLevel& level = kv.second;
        uint64_t sum = 0;
        uint32_t idx = level.head;
        while (idx != UINT32_MAX) {
            sum += pool_[idx].quantity;
            idx = pool_[idx].next;
        }
        if (sum > 0) out.emplace_back(price, sum);
    }
    return out;
}

std::vector<std::pair<double,uint64_t>> OrderBook::snapshot_asks() const {
    std::vector<std::pair<double,uint64_t>> out;
    out.reserve(asks_.size());
    for (const auto& kv : asks_) {
        double price = kv.first;
        const PriceLevel& level = kv.second;
        uint64_t sum = 0;
        uint32_t idx = level.head;
        while (idx != UINT32_MAX) {
            sum += pool_[idx].quantity;
            idx = pool_[idx].next;
        }
        if (sum > 0) out.emplace_back(price, sum);
    }
    return out;
}

// ---------------- Add residual to book ----------------

// Convert external Order to PoolOrder and rest it on the appropriate side/price level.
void OrderBook::add_to_book(const Order& o) {
    uint32_t idx = pool_.allocate();
    PoolOrder& po = pool_[idx];

    po.order_id  = o.order_id;
    po.user_id   = o.user_id;
    po.side      = (o.side == Side::Buy ? 0u : 1u);
    po.price     = o.price;
    po.quantity  = o.quantity;
    po.timestamp = o.ts_ns;

    if (o.side == Side::Buy) {
        auto [it, inserted] = bids_.try_emplace(o.price);
        append_to_level(it->second, idx);
    } else {
        auto [it, inserted] = asks_.try_emplace(o.price);
        append_to_level(it->second, idx);
    }

    order_index_[o.order_id] = OrderRef{o.side, o.price, idx};
}

// ---------------- Matching engine ----------------

// Cross incoming buy against best asks while marketable (ask_price <= incoming.price).
void OrderBook::match_buy(Order& incoming, std::vector<Trade>& out_trades) {
    while (incoming.quantity > 0 && !asks_.empty()) {
        auto best_it = asks_.begin();
        double ask_price = best_it->first;
        if (ask_price > incoming.price) break;

        PriceLevel& level = best_it->second;
        uint32_t idx = level.head;

        while (idx != UINT32_MAX && incoming.quantity > 0) {
            PoolOrder& resting = pool_[idx];
            uint32_t next = resting.next;

            uint64_t qty = std::min(incoming.quantity, resting.quantity);

            Trade tr;
            tr.trade_id       = allocate_trade_id();
            tr.buy_order_id   = incoming.order_id;
            tr.sell_order_id  = resting.order_id;
            tr.price          = ask_price;
            tr.quantity       = qty;
            tr.instrument_id = incoming.instrument_id;
            tr.buy_user_id    = incoming.user_id;
            tr.sell_user_id   = resting.user_id;

            out_trades.push_back(tr);


            incoming.quantity -= qty;
            resting.quantity  -= qty;

            if (resting.quantity == 0) {
                order_index_.erase(resting.order_id);
                unlink_from_level(level, idx);
                pool_.release(idx);
            }

            idx = next;
        }

        if (level_empty(level))
            asks_.erase(best_it);
    }
}

// Cross incoming sell against best bids while marketable (bid_price >= incoming.price).
void OrderBook::match_sell(Order& incoming, std::vector<Trade>& out_trades) {
    while (incoming.quantity > 0 && !bids_.empty()) {
        auto best_it = bids_.begin();
        double bid_price = best_it->first;
        if (bid_price < incoming.price) break;

        PriceLevel& level = best_it->second;
        uint32_t idx = level.head;

        while (idx != UINT32_MAX && incoming.quantity > 0) {
            PoolOrder& resting = pool_[idx];
            uint32_t next = resting.next;

            uint64_t qty = std::min(incoming.quantity, resting.quantity);

            Trade tr;
            tr.trade_id       = allocate_trade_id();
            tr.buy_order_id   = resting.order_id;
            tr.sell_order_id  = incoming.order_id;
            tr.price          = bid_price;
            tr.quantity       = qty;
            tr.instrument_id = incoming.instrument_id;
            tr.buy_user_id    = resting.user_id;
            tr.sell_user_id   = incoming.user_id;

            out_trades.push_back(tr);


            incoming.quantity -= qty;
            resting.quantity  -= qty;

            if (resting.quantity == 0) {
                order_index_.erase(resting.order_id);
                unlink_from_level(level, idx);
                pool_.release(idx);
            }

            idx = next;
        }

        if (level_empty(level))
            bids_.erase(best_it);
    }
}

} // namespace quant
