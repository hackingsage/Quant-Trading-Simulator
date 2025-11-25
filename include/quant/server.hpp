#pragma once
#include <atomic>
#include <thread>
#include <unordered_map>
#include "quant/messages.hpp"
#include "quant/order_book.hpp"
#include "quant/spsc_queue.hpp"
#include "quant/pnl.hpp"

namespace quant {

// MatchingServer
//
// Orchestrates intake of client messages, matching via OrderBook,
// PnL attribution, and emission of server telemetry. Runs an engine
// loop thread that drains the input queue and fills the output queue.
class MatchingServer {
public:
    // Construct with bounded SPSC queues for client->server and server->client messages.
    MatchingServer(std::size_t in_capacity = 4096, std::size_t out_capacity = 4096);
    // Join engine thread and release resources.
    ~MatchingServer();

    // Spawn the engine loop thread.
    void start();
    // Stop the engine loop and join the thread.
    void stop();

    // Non-blocking enqueue of a new order message; returns false if input queue is full.
    bool submit_new_order(const MsgNewOrder& m);
    // Non-blocking enqueue of a cancel request; returns false if input queue is full.
    bool submit_cancel(const MsgCancel& m);

    // Non-blocking dequeue of next server message for network/UI; returns false if none.
    bool get_next_server_message(ServerMessage& out_msg);

private:
    // Single-threaded engine: drain input, apply to OrderBook, update PnL, emit outputs.
    void engine_loop();

private:
    // Engine lifecycle state shared with worker thread.
    std::atomic<bool> running_;
    // Client -> Engine bounded queue (single producer: API/network, single consumer: engine thread).
    SPSCQueue<ClientMessage> in_queue_;
    // Engine -> Network/UI bounded queue.
    SPSCQueue<ServerMessage> out_queue_;
    // Price-time priority order book for matching.
    OrderBook book_;
    // Dedicated engine loop thread.
    std::thread engine_thread_;

    // --- PnL tracking for a specific user (e.g. user_id = 1 from React UI) ---
    // Primary user id observed by UI for PnL streaming.
    uint64_t tracked_user_id_;
    // PnL engine for tracked user.
    PnLEngine pnl_;

    // --- NEW: PnL tracking for BS bot (user_id = 9999) ---
    // PnL engine for BS bot (user_id = 9999) to attribute strategy PnL separately.
    PnLEngine bs_pnl_;

    struct UserPnL {
        double cash = 0.0;
        uint64_t position = 0;
        double realized = 0.0;
    };

    // Lightweight per-user attribution for cash/position/realized, complements PnLEngines.
    std::unordered_map<uint64_t, UserPnL> user_pnl_;
    
    // Map order_id -> user_id (for resting orders & attribution)
    // Map resting order_id -> user_id for attribution on fills and cancels.
    std::unordered_map<uint64_t, uint64_t> order_user_;
};

} // namespace quant
