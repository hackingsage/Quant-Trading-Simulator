#include "quant/pnl.hpp"
#include <cmath>

namespace quant {

PnLEngine::PnLEngine(uint64_t user_id)
    : user_id_(user_id),
      position_(0.0),
      avg_price_(0.0),
      realized_pnl_(0.0),
      unrealized_pnl_(0.0),
      last_mid_(0.0) {}

void PnLEngine::on_trade(bool user_is_buy, double price, uint64_t qty) {
    std::lock_guard<std::mutex> g(mtx_);
    double signed_qty = user_is_buy ? double(qty) : -double(qty);

    // If closing (opposite sign), compute realized PnL on closed portion
    if (position_ != 0.0 && (position_ * signed_qty) < 0.0) {
        // qty that actually closes existing position
        double close_qty = std::min(std::abs(position_), std::abs(signed_qty));
        // realized = (sell_price - buy_price) * closed_qty
        // determine whether the closing trade is selling existing long or buying to cover short
        if (position_ > 0.0) {
            // we had a long; closing by selling at 'price'
            realized_pnl_ += (price - avg_price_) * close_qty;
        } else {
            // we had a short; closing by buying at 'price'
            realized_pnl_ += (avg_price_ - price) * close_qty;
        }
        // adjust remaining signed_qty after closing
        signed_qty = (std::abs(signed_qty) > close_qty)
                     ? (signed_qty > 0 ? (signed_qty - close_qty) : (signed_qty + close_qty))
                     : 0.0;
        // update position
        if (std::abs(position_) <= close_qty) {
            position_ = 0.0;
            avg_price_ = 0.0;
        } else {
            // reduce existing position magnitude
            if (position_ > 0) position_ = position_ - close_qty;
            else position_ = position_ + close_qty;
        }
    }

    // If there is remaining signed_qty with same sign as position (or new position), update avg price
    if (signed_qty != 0.0) {
        // If we had zero position, simply set avg_price
        if (position_ == 0.0) {
            avg_price_ = price;
            position_ = signed_qty;
        } else if ((position_ > 0.0 && signed_qty > 0.0) || (position_ < 0.0 && signed_qty < 0.0)) {
            double new_pos = position_ + signed_qty;
            // compute new VWAP
            avg_price_ = (avg_price_ * std::abs(position_) + price * std::abs(signed_qty)) / (std::abs(new_pos));
            position_ = new_pos;
        } else {
            // signed_qty opposite sign but fully absorbed by previous closing branch; unlikely to reach here
            position_ += signed_qty;
            if (position_ == 0.0) avg_price_ = 0.0;
        }
    }

    // update unrealized with last_mid_
    if (last_mid_ > 0.0) {
        if (position_ > 0.0) unrealized_pnl_ = (last_mid_ - avg_price_) * std::abs(position_);
        else unrealized_pnl_ = (avg_price_ - last_mid_) * std::abs(position_);
    }
}

void PnLEngine::on_midprice(double mid) {
    std::lock_guard<std::mutex> g(mtx_);
    last_mid_ = mid;
    if (position_ == 0.0) {
        unrealized_pnl_ = 0.0;
    } else if (position_ > 0.0) {
        unrealized_pnl_ = (last_mid_ - avg_price_) * std::abs(position_);
    } else {
        unrealized_pnl_ = (avg_price_ - last_mid_) * std::abs(position_);
    }
}

quant::PnLUpdate PnLEngine::get() const {
    std::lock_guard<std::mutex> g(mtx_);
    quant::PnLUpdate out;
    out.realized   = realized_pnl_;
    out.unrealized = unrealized_pnl_;
    out.position   = position_;
    out.avg_price  = avg_price_;
    out.equity     = realized_pnl_ + unrealized_pnl_;
    return out;
}

} // namespace quant
