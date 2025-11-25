#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include "quant/order_pool.hpp"
#include "quant/messages.hpp"

namespace quant {

// OrderBook
//
// In-memory limit order book with price-time priority. Maintains bid/ask
// maps of price levels and a pool-backed intrusive queue for orders.
// Provides order submission, cancellation, matching, and top-of-book snapshots.
class OrderBook {
public:
    // Construct an order book for a given symbol. Symbol is used only for identification
    // and does not affect matching rules.
    explicit OrderBook(const std::string& symbol);

    // Return the instrument symbol handled by this book.
    const std::string& symbol() const { return symbol_; }

    // Submit a limit order. Returns assigned order_id. Any immediate matches
    // generate Trade entries appended to out_trades; remaining quantity rests.
    uint64_t submit_limit_order(const Order& order, std::vector<Trade>& out_trades);
    // Cancel a resting order by id. Returns true if the order was found and removed.
    bool     cancel_order(uint64_t order_id);

    // Compute current top of book (best bid/ask consolidated quantities).
    TopOfBook top_of_book() const;

    // Number of resting orders currently indexed.
    std::size_t size() const { return order_index_.size(); }

    // Snapshot bid price levels as (price, aggregate_qty) sorted by price desc.
    std::vector<std::pair<double, uint64_t>> snapshot_bids() const;
    // Snapshot ask price levels as (price, aggregate_qty) sorted by price asc.
    std::vector<std::pair<double, uint64_t>> snapshot_asks() const;

private:
    // FIFO queue endpoints (indices into OrderPool) for a single price level.
    struct PriceLevel {
        uint32_t head = UINT32_MAX;
        uint32_t tail = UINT32_MAX;
    };

    using BidMap = std::map<double, PriceLevel, std::greater<double>>;
    using AskMap = std::map<double, PriceLevel, std::less<double>>;

    // Lightweight reference for fast lookup: maps order_id -> (side, price, pool index).
    struct OrderRef {
        Side     side;
        double   price;
        uint32_t idx;
    };

    std::string symbol_;
    BidMap bids_;
    AskMap asks_;
    std::unordered_map<uint64_t, OrderRef> order_index_;

    uint64_t next_order_id_  = 1;
    uint64_t next_trade_id_  = 1;
    uint64_t next_timestamp_ = 1;

    OrderPool pool_;

    uint64_t allocate_order_id();
    uint64_t allocate_trade_id();
    uint64_t allocate_timestamp();

    // Cross an incoming buy against best asks while marketable.
    void match_buy(Order& incoming, std::vector<Trade>& out_trades);
    // Cross an incoming sell against best bids while marketable.
    void match_sell(Order& incoming, std::vector<Trade>& out_trades);
    // Rest remaining quantity on the appropriate side/price level.
    void add_to_book(const Order& o);

    // Link an order node at the tail of a price level queue.
    void append_to_level(PriceLevel& level, uint32_t idx);
    // Unlink an order node from a price level queue, maintaining head/tail.
    void unlink_from_level(PriceLevel& level, uint32_t idx);
    // Check if a price level has no orders.
    bool level_empty(const PriceLevel& level) const;
};

} // namespace quant
