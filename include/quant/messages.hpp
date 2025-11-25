#pragma once
#include <cstdint>

namespace quant {

enum class Side : uint8_t {Buy = 0, Sell = 1};

enum MsgType : uint8_t {
    NEW_ORDER  = 1,
    CANCEL     = 2,
    TRADE      = 3,
    ACK        = 4,
    TOB        = 5,
    L2_UPDATE  = 6,
    PNL_UPDATE = 7
};

// ------------ Client → Engine ------------

struct MsgNewOrder {
    uint64_t user_id;
    uint8_t  side;       // 0=Buy, 1=Sell
    double   price;
    uint64_t quantity;
    uint32_t instrument_id; // ADDED earlier
};

struct Order {
    uint64_t order_id;   // 0 => let engine assign
    uint64_t user_id;
    Side     side;
    double   price;
    uint64_t quantity;
    uint64_t ts_ns;
    uint64_t instrument_id;
    uint64_t remaining;
};

struct MsgCancel {
    uint32_t user_id = 0;
    uint64_t order_id;
};

struct ClientMessage {
    MsgType      type;
    MsgNewOrder  new_order;
    MsgCancel    cancel;
};

// ------------ Engine → Client ------------

struct Trade {
    uint64_t trade_id;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    double   price;
    uint64_t quantity;
    uint64_t instrument_id;
    uint64_t ts_ns = 0;
    // NEW: who traded
    uint64_t buy_user_id;
    uint64_t sell_user_id;
};

struct Ack {
    uint8_t  status;        // ACK / NACK
    uint8_t  type;          // NEW_ORDER / CANCEL
    uint64_t order_id;
};

struct TopOfBook {
    bool     has_bid = false;
    bool     has_ask = false;
    double   bid_price = 0;
    uint64_t bid_quantity = 0;
    double   ask_price = 0;
    uint64_t ask_quantity = 0;
};

struct L2Update {
    uint8_t  side;     // 0=bid, 1=ask
    double   price;
    uint64_t quantity;
};

struct PnLUpdate {
    uint32_t user_id = 0;
    double realized;
    double unrealized;
    double position;
    double avg_price;
    double equity;
};

// SERVER MESSAGE
struct ServerMessage {
    MsgType     type;

    Trade       trade;
    Ack         ack;
    TopOfBook   tob;
    L2Update    l2;
    PnLUpdate   pnl;

    // NEW field to identify BS bot trades
    uint8_t     is_bot_trade = 0;
};

} // namespace quant
