#pragma once
#include "quant/messages.hpp" // use the protocol PnLUpdate defined there
#include <cstdint>
#include <mutex>

namespace quant {

/**
 * PnLEngine
 *
 * Keeps per-user PnL state and exposes snapshots using the protocol type
 * quant::PnLUpdate (defined in messages.hpp).
 *
 * Note: messages.hpp already defines the PnLUpdate struct used on the wire,
 * so we must NOT redefine it here (avoid duplicate-definition errors).
 */
class PnLEngine {
public:
    explicit PnLEngine(uint64_t user_id);

    // Called when a trade happens (trade fill for this user).
    // user_is_buy == true => this user bought qty at price
    // user_is_buy == false => this user sold qty at price
    void on_trade(bool user_is_buy, double price, uint64_t qty);

    // Called when mid/TOB updates (update unrealized PnL)
    void on_midprice(double mid);

    // Get latest snapshot (thread-safe copy)
    quant::PnLUpdate get() const;

private:
    uint64_t user_id_;
    mutable std::mutex mtx_;

    double position_;     // +long, -short (signed)
    double avg_price_;    // VWAP of open position (price average sign-aware)
    double realized_pnl_;
    double unrealized_pnl_;
    double last_mid_;
};

} // namespace quant
