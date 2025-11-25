#pragma once
#include <atomic>
#include <thread>
#include <mutex>
#include <random>
#include <chrono>
#include "quant/server.hpp"   // MsgNewOrder, MsgCancel, MatchingServer, ServerMessage
#include "quant/messages.hpp"

namespace quant {

enum OptionType { Call = 0, Put = 1 };

// Configuration for BS-based quoting/hedging strategy.
// - user_id used for attribution; instrument ids for underlying/option legs
// - expiry_seconds (seconds), r annualized, iv annualized implied volatility
// - spread is absolute around theoretical; qty is per-leg size
// - hedge_tolerance controls delta threshold for hedging; inventory caps bound exposure
struct BSBotConfig {
    uint64_t user_id = 9999;       // user id used for bot orders
    uint32_t underlying_instrument = 1; // id for underlying
    uint32_t option_instrument = 2;     // id for option instrument
    OptionType opt_type = OptionType::Call;
    double strike = 100.0;
    double expiry_seconds = 3600.0; // time to expiry (seconds)
    double r = 0.0;                 // risk-free rate
    double iv = 0.20;               // initial implied vol (annualized)
    double spread = 0.02;           // absolute spread around theoretical price
    double qty = 5.0;               // order quantity for option legs
    double hedge_tolerance = 0.1;   // acceptable net delta before hedging
    double max_option_inventory = 1000.0;
    double min_price = 0.0001;
    double max_price = 1e7;
    double update_interval_s = 0.2; // how often bot updates quotes
};

// BSBot: quotes two-sided markets around Black–Scholes fair value and hedges delta
// when inventory deviates beyond tolerance. Integrates with MatchingServer for order I/O.
class BSBot {
public:
    BSBot(MatchingServer* engine, const BSBotConfig& cfg);
    ~BSBot();

    // Launch/stop control thread running the quoting/hedging loop.
    void start();
    void stop();

    // optionally allow runtime updates
    // Update implied volatility used for theoretical pricing at runtime.
    void set_iv(double iv);

private:
    // Control loop: compute fair (BS), build bid/ask with spread/skew, submit/replace orders,
    // monitor fills and delta; place hedges when deviation exceeds hedge_tolerance.
    void thread_loop();
    // BS closed-form price (no dividends). tau in years; r, sigma annualized.
    double bs_price(double S, double K, double r, double sigma, double tau, bool is_call) const;
    // Black–Scholes delta for call/put. Used for hedge sizing.
    double bs_delta(double S, double K, double r, double sigma, double tau, bool is_call) const;
    // Standard normal pdf/cdf helpers for BS analytics.
    double norm_pdf(double x) const;
    double norm_cdf(double x) const;

    // order helpers
    // Submit a limit order via MatchingServer and return assigned order id.
    uint64_t post_limit_order(uint32_t instrument, uint8_t side, double price, uint64_t qty);
    // Cancel a previously posted order id (if still resting).
    void cancel_order(uint64_t order_id);

    MatchingServer* engine_;
    BSBotConfig cfg_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::mutex mtx_;

    // Active order tracking for quote maintenance and hedge lifecycle.
    std::vector<uint64_t> active_option_orders_;
    std::vector<uint64_t> active_hedge_orders_;

    // Inventory tracking: option position and underlying hedge; last mid for MTM/skewing.
    double option_inventory_ = 0.0;
    double hedge_inventory_ = 0.0; // position in underlying
    double last_mid_ = 0.0;

    std::mt19937 rng_;
};

} // namespace quant
