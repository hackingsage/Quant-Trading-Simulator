#include "quant/bs_bot.hpp"
#include <cmath>
#include <iostream>
#include <algorithm>
#include "quant/bs_bot.hpp"


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


namespace quant {

// -- helpers: normal pdf/cdf (use std::erf)
double BSBot::norm_pdf(double x) const {
    return std::exp(-0.5 * x * x) / std::sqrt(2.0 * M_PI);
}
double BSBot::norm_cdf(double x) const {
    return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
}

double BSBot::bs_price(double S, double K, double r, double sigma, double tau, bool is_call) const {
    if (S <= 0.0 || K <= 0.0 || sigma <= 0.0 || tau <= 0.0) {
        // handle degenerate cases
        double intrinsic = is_call ? std::max(0.0, S - K) : std::max(0.0, K - S);
        return intrinsic;
    }
    double sqrt_tau = std::sqrt(tau);
    double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * tau) / (sigma * sqrt_tau);
    double d2 = d1 - sigma * sqrt_tau;
    if (is_call) {
        return S * norm_cdf(d1) - K * std::exp(-r * tau) * norm_cdf(d2);
    } else {
        return K * std::exp(-r * tau) * norm_cdf(-d2) - S * norm_cdf(-d1);
    }
}

double BSBot::bs_delta(double S, double K, double r, double sigma, double tau, bool is_call) const {
    if (S <= 0.0 || K <= 0.0 || sigma <= 0.0 || tau <= 0.0) {
        if (is_call) return S > K ? 1.0 : 0.0;
        else return S > K ? 0.0 : -1.0;
    }
    double sqrt_tau = std::sqrt(tau);
    double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * tau) / (sigma * sqrt_tau);
    double nd1 = norm_cdf(d1);
    return is_call ? nd1 : (nd1 - 1.0);
}

BSBot::BSBot(MatchingServer* engine, const BSBotConfig& cfg)
    : engine_(engine), cfg_(cfg), running_(false), rng_(std::random_device{}())
{}

BSBot::~BSBot() {
    stop();
}

void BSBot::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&BSBot::thread_loop, this);
}

void BSBot::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void BSBot::set_iv(double iv) {
    std::lock_guard<std::mutex> g(mtx_);
    cfg_.iv = iv;
}

uint64_t BSBot::post_limit_order(uint32_t instrument, uint8_t side, double price, uint64_t qty) {
    // safety/clamping
    if (price < cfg_.min_price) price = cfg_.min_price;
    if (price > cfg_.max_price) price = cfg_.max_price;
    MsgNewOrder mo{};
    mo.user_id = cfg_.user_id;
    mo.instrument_id = instrument;
    mo.side = side;
    mo.price = price;
    mo.quantity = qty;
    // if MsgNewOrder has instrument id, set it here; otherwise adapt
    // mo.instrument_id = instrument; // uncomment if your struct has it
    bool pushed = engine_->submit_new_order(mo);
    if (!pushed) return 0;
    // MatchingServer currently returns assigned order id via ACK — to track exact IDs you'd need the ACK handler.
    // For simplicity we'll store 0 placeholders (and rely on engine->book snapshot for position)
    return 0;
}

void BSBot::cancel_order(uint64_t order_id) {
    if (order_id == 0) return;
    MsgCancel mc{};
    mc.order_id = order_id;
    engine_->submit_cancel(mc);
}

void BSBot::thread_loop() {
    using namespace std::chrono;

    auto last_update = steady_clock::now();
    auto last_print  = steady_clock::now();   // <-- NEW: last time we printed PnL

    while (running_) {

        // --------- Consume TOB and TRADE messages ---------
        ServerMessage sm;
        while (engine_->get_next_server_message(sm)) {
            if (sm.type == TOB) {
                double mid = 0.0;
                if (sm.tob.bid_price > 0.0 && sm.tob.ask_price > 0.0)
                    mid = 0.5 * (sm.tob.bid_price + sm.tob.ask_price);
                else if (sm.tob.bid_price > 0.0)
                    mid = sm.tob.bid_price;
                else if (sm.tob.ask_price > 0.0)
                    mid = sm.tob.ask_price;

                last_mid_ = mid;
            }
            else if (sm.type == TRADE) {
                if (sm.trade.instrument_id == cfg_.option_instrument) {

                    if (sm.trade.buy_user_id == cfg_.user_id) {
                        // Bot bought an option → increase option inventory
                        option_inventory_ += (double)sm.trade.quantity;
                    }
                    else if (sm.trade.sell_user_id == cfg_.user_id) {
                        // Bot sold an option → decrease inventory
                        option_inventory_ -= (double)sm.trade.quantity;
                    }
                }

                // Hedge trade?
                if (sm.trade.instrument_id == cfg_.underlying_instrument) {

                    if (sm.trade.buy_user_id == cfg_.user_id) {
                        hedge_inventory_ += (double)sm.trade.quantity;
                    }
                    else if (sm.trade.sell_user_id == cfg_.user_id) {
                        hedge_inventory_ -= (double)sm.trade.quantity;
                    }
                }
            }

        }

        // --------- Update interval control (bot logic) ---------
        auto now = steady_clock::now();
        double elapsed = duration_cast<duration<double>>(now - last_update).count();
        if (elapsed < cfg_.update_interval_s) {
            std::this_thread::sleep_for(milliseconds(10));
            continue;
        }
        last_update = now;

        // Skip if no market mid
        double S = last_mid_;
        if (S <= 0.0) {
            std::this_thread::sleep_for(milliseconds(20));
            continue;
        }

        // ======== Black–Scholes Model ========
        double tau = std::max(1e-6, cfg_.expiry_seconds / 365.0);
        double theo, delta;

        {
            std::lock_guard<std::mutex> g(mtx_);
            theo  = bs_price(S, cfg_.strike, cfg_.r, cfg_.iv, tau,
                             cfg_.opt_type == OptionType::Call);
            delta = bs_delta(S, cfg_.strike, cfg_.r, cfg_.iv, tau,
                             cfg_.opt_type == OptionType::Call);
        }

        double bid_price = std::max(cfg_.min_price, theo - cfg_.spread * 0.5);
        double ask_price = std::min(cfg_.max_price, theo + cfg_.spread * 0.5);
        double max_rel   = std::max(1.0, S * 10.0);

        if (bid_price > max_rel) bid_price = max_rel;
        if (ask_price > max_rel) ask_price = max_rel;

        // Cancel old option orders
        for (auto id : active_option_orders_) cancel_order(id);
        active_option_orders_.clear();

        uint64_t q = (uint64_t)std::max(1.0, cfg_.qty);

        post_limit_order(cfg_.option_instrument, 0, bid_price, q);
        post_limit_order(cfg_.option_instrument, 1, ask_price, q);

        // ======== Delta Hedging ========
        double target_hedge = -delta * option_inventory_;
        double need = target_hedge - hedge_inventory_;

        if (std::abs(need) > cfg_.hedge_tolerance) {
            uint8_t side = need > 0 ? 0 : 1; 
            double price = (side == 0 ? S + 0.01 : S - 0.01);
            price = std::clamp(price, cfg_.min_price, cfg_.max_price);
            uint64_t hedge_qty = (uint64_t)std::min(std::abs(need), 100.0);

            post_limit_order(cfg_.underlying_instrument, side, price, hedge_qty);

            hedge_inventory_ += (side == 0 ? (double)hedge_qty : -(double)hedge_qty);
        }

        // ======== PRINT BOT PNL EVERY 1 SECOND ========
        double print_elapsed = duration_cast<duration<double>>(now - last_print).count();
        if (print_elapsed >= 1.0) {          // PRINT EVERY 1 SEC
            last_print = now;

            std::cout << "[BS-BOT] S=" << S << " | theo=" << theo << " | delta=" << delta << " | hedge_inv=" << hedge_inventory_ << " | opt_inv=" << option_inventory_ << std::endl;
        }
    }
}


} // namespace quant
