#include "quant/server.hpp"
#include <vector>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <iostream>

namespace quant {

MatchingServer::MatchingServer(std::size_t in_capacity, std::size_t out_capacity)
    : running_(false),
      in_queue_(in_capacity),
      out_queue_(out_capacity),
      book_("FOO"),
      tracked_user_id_(1),
      pnl_(tracked_user_id_),
      bs_pnl_(9999) // BS bot user id
{}

MatchingServer::~MatchingServer() {
    stop();
}

void MatchingServer::start() {
    if (running_) return;
    running_ = true;
    engine_thread_ = std::thread(&MatchingServer::engine_loop, this);
}

void MatchingServer::stop() {
    if (!running_) return;
    running_ = false;
    if (engine_thread_.joinable()) engine_thread_.join();
}

bool MatchingServer::submit_new_order(const MsgNewOrder& m) {
    ClientMessage cm{};
    cm.type = NEW_ORDER;
    cm.new_order = m;
    return in_queue_.push(cm);
}

bool MatchingServer::submit_cancel(const MsgCancel& m) {
    ClientMessage cm{};
    cm.type = CANCEL;
    cm.cancel = m;
    return in_queue_.push(cm);
}

bool MatchingServer::get_next_server_message(ServerMessage& out_msg) {
    return out_queue_.pop(out_msg);
}

// --- BS bot user id (must match BSBotConfig.user_id) ---
static constexpr uint64_t BS_BOT_USER_ID = 9999;

void emit_trades(const std::vector<Trade>& trades,
                 SPSCQueue<ServerMessage>& out_queue_) {
    for (const auto& t : trades) {
        ServerMessage sm{};
        sm.type  = TRADE;
        sm.trade = t;

        // If you want, you can mark bot trades here later:
        // sm.is_bot_trade =
        //     (t.buy_user_id  == BS_BOT_USER_ID ||
        //      t.sell_user_id == BS_BOT_USER_ID) ? 1 : 0;

        out_queue_.push(sm);
    }
}

static void emit_ack(MsgType type, uint64_t order_id, bool ok,
                     SPSCQueue<ServerMessage>& out_q) {
    ServerMessage sm{};
    sm.type = ACK;
    sm.ack.status   = ok ? 0 : 1; // 0=OK,1=ERROR
    sm.ack.type     = static_cast<uint8_t>(type);
    sm.ack.order_id = order_id;
    out_q.push(sm);
}

void MatchingServer::engine_loop() {
    TopOfBook last_tob{};
    bool have_last_tob = false;

    // Process up to BATCH_SIZE client messages per iteration to bound latency and work per tick.
    constexpr std::size_t BATCH_SIZE = 1024;

    while (running_) {
        std::size_t processed = 0;

        while (processed < BATCH_SIZE) {
            ClientMessage cm;
            if (!in_queue_.pop(cm)) break;
            ++processed;

            // Capture previous L2 snapshots to emit minimal diffs after processing.
            auto prev_bids = book_.snapshot_bids();
            auto prev_asks = book_.snapshot_asks();

            std::vector<Trade> trades;
            trades.reserve(8);

            if (cm.type == NEW_ORDER) {
                // Construct engine Order from client message; engine assigns id/timestamp as needed.
                Order o;
                o.order_id  = 0;
                o.user_id   = cm.new_order.user_id;
                o.instrument_id = cm.new_order.instrument_id;
                o.side      = (cm.new_order.side == 0 ? Side::Buy : Side::Sell);
                o.price     = cm.new_order.price;
                o.quantity  = cm.new_order.quantity;
                o.remaining = o.quantity;
                o.ts_ns = 0;

                uint64_t assigned_id = book_.submit_limit_order(o, trades);

                // Map resting order id -> user_id (for attribution)
                if (assigned_id != 0) {
                    order_user_[assigned_id] = cm.new_order.user_id;
                }

                // ----- PnL attribution for trades -----
                for (const auto& tr : trades) {
                    // Determine per-trade attribution: whether tracked UI user and/or BS bot acted as buyer/seller.
                    bool user_is_buy  = false;
                    bool user_is_sell = false;

                    bool bot_is_buy   = false;
                    bool bot_is_sell  = false;

                    // Incoming order side for UI-tracked user
                    if (cm.new_order.user_id == tracked_user_id_) {
                        if (cm.new_order.side == 0) user_is_buy  = true;
                        else                        user_is_sell = true;
                    }
                    // Incoming side for BS bot
                    if (cm.new_order.user_id == BS_BOT_USER_ID) {
                        if (cm.new_order.side == 0) bot_is_buy  = true;
                        else                        bot_is_sell = true;
                    }

                    // Resting side (look up via order_user_ map)
                    auto itB = order_user_.find(tr.buy_order_id);
                    if (itB != order_user_.end()) {
                        if (itB->second == tracked_user_id_) {
                            user_is_buy = true;
                            user_is_sell = false;
                        }
                        if (itB->second == BS_BOT_USER_ID) {
                            bot_is_buy = true;
                            bot_is_sell = false;
                        }
                    }

                    auto itS = order_user_.find(tr.sell_order_id);
                    if (itS != order_user_.end()) {
                        if (itS->second == tracked_user_id_) {
                            user_is_sell = true;
                            user_is_buy  = false;
                        }
                        if (itS->second == BS_BOT_USER_ID) {
                            bot_is_sell = true;
                            bot_is_buy  = false;
                        }
                    }

                    // --- PnL for UI user (e.g. user_id = 1) ---
                    if (user_is_buy || user_is_sell) {
                        pnl_.on_trade(user_is_buy, tr.price, tr.quantity);
                        PnLUpdate pu = pnl_.get();
                        pu.user_id = static_cast<uint32_t>(tracked_user_id_);

                        ServerMessage sm_p{};
                        sm_p.type = PNL_UPDATE;
                        sm_p.pnl  = pu;
                        out_queue_.push(sm_p);
                    }

                    // --- PnL for BS bot (user_id = 9999) ---
                    if (bot_is_buy || bot_is_sell) {
                        bs_pnl_.on_trade(bot_is_buy, tr.price, tr.quantity);
                        PnLUpdate pu_b = bs_pnl_.get();
                        pu_b.user_id = static_cast<uint32_t>(BS_BOT_USER_ID);

                        ServerMessage sm_p{};
                        sm_p.type = PNL_UPDATE;
                        sm_p.pnl  = pu_b;
                        out_queue_.push(sm_p);
                    }
                }

                emit_trades(trades, out_queue_);
                emit_ack(NEW_ORDER, assigned_id, true, out_queue_);
            } else if (cm.type == CANCEL) {
                bool ok = book_.cancel_order(cm.cancel.order_id);
                if (ok) {
                    order_user_.erase(cm.cancel.order_id);
                }
                emit_ack(CANCEL, cm.cancel.order_id, ok, out_queue_);
            }

            // ----- Top of book + PnL (midprice) -----
            // Emit TOB changes only when the top-of-book differs from last snapshot; mid price drives PnL.
            TopOfBook tob = book_.top_of_book();
            bool tob_changed = false;
            if (!have_last_tob) {
                tob_changed = true;
                have_last_tob = true;
            } else {
                if (tob.has_bid != last_tob.has_bid ||
                    tob.has_ask != last_tob.has_ask ||
                    tob.bid_price != last_tob.bid_price ||
                    tob.bid_quantity != last_tob.bid_quantity ||
                    tob.ask_price != last_tob.ask_price ||
                    tob.ask_quantity != last_tob.ask_quantity) {
                    tob_changed = true;
                }
            }

            if (tob_changed) {
                last_tob = tob;
                ServerMessage sm{};
                sm.type          = TOB;
                sm.tob.bid_price = tob.has_bid ? tob.bid_price : 0.0;
                sm.tob.bid_quantity   = tob.has_bid ? tob.bid_quantity : 0;
                sm.tob.ask_price = tob.has_ask ? tob.ask_price : 0.0;
                sm.tob.ask_quantity   = tob.has_ask ? tob.ask_quantity : 0;
                out_queue_.push(sm);

                // Midprice for PnL
                double mid = 0.0;
                if (tob.has_bid && tob.has_ask) {
                    mid = 0.5 * (tob.bid_price + tob.ask_price);
                } else if (tob.has_bid) {
                    mid = tob.bid_price;
                } else if (tob.has_ask) {
                    mid = tob.ask_price;
                }

                if (mid > 0.0) {
                    // UI user
                    pnl_.on_midprice(mid);
                    PnLUpdate pu = pnl_.get();
                    pu.user_id = static_cast<uint32_t>(tracked_user_id_);

                    ServerMessage sm_p{};
                    sm_p.type = PNL_UPDATE;
                    sm_p.pnl  = pu;
                    out_queue_.push(sm_p);

                    // BS bot
                    bs_pnl_.on_midprice(mid);
                    PnLUpdate pu_b = bs_pnl_.get();
                    pu_b.user_id = static_cast<uint32_t>(BS_BOT_USER_ID);

                    ServerMessage sm_pb{};
                    sm_pb.type = PNL_UPDATE;
                    sm_pb.pnl  = pu_b;
                    out_queue_.push(sm_pb);
                }
            }

            // ----- L2 diffs (for order book) -----
            auto new_bids = book_.snapshot_bids();
            auto new_asks = book_.snapshot_asks();

            // Compute per-price quantity diffs between snapshots and emit L2_UPDATE frames per change.
            auto diff_side = [&](const std::vector<std::pair<double,uint64_t>>& before,
                                 const std::vector<std::pair<double,uint64_t>>& after,
                                 uint8_t side_flag) {
                std::unordered_map<double,uint64_t> prev_map;
                for (auto& p : before) prev_map[p.first] = p.second;
                std::unordered_map<double,uint64_t> new_map;
                for (auto& p : after) new_map[p.first] = p.second;

                std::unordered_map<double,bool> seen;
                for (auto& kv : prev_map) seen[kv.first] = true;
                for (auto& kv : new_map)  seen[kv.first] = true;

                for (auto& kv : seen) {
                    double price   = kv.first;
                    uint64_t old_q = prev_map.count(price) ? prev_map[price] : 0;
                    uint64_t new_q = new_map.count(price) ? new_map[price] : 0;
                    if (old_q != new_q) {
                        ServerMessage sm{};
                        sm.type = L2_UPDATE;
                        sm.l2.side = side_flag;
                        sm.l2.price = price;
                        sm.l2.quantity = new_q;
                        out_queue_.push(sm);
                    }
                }
            };

            diff_side(prev_bids, new_bids, 0);
            diff_side(prev_asks, new_asks, 1);
        }

        // Backoff briefly if no work was processed to avoid busy spinning.
        if (processed == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

} // namespace quant
