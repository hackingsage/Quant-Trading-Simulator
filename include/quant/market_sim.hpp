#pragma once
#include <atomic>
#include <thread>
#include <random>
#include "quant/server.hpp"
#include "quant/messages.hpp"

namespace quant {

// GBM-driven market simulator that emits synthetic order flow to MatchingServer.
// - Evolves mid/reference price via GBM; quantizes to tick_size.
// - Periodically places buy/sell limit orders around the evolving price.
// - Designed for determinism via fixed RNG seed and stable time stepping.
class MarketSimulator {
public:
    // Construct simulator and precompute drift/vol terms for stable stepping.
    MarketSimulator(MatchingServer* engine,
                    double s0          = 10000.0,
                    double mu          = 0.2,    // drift (annualized-ish, not super important here)
                    double sigma       = 0.2,    // vol
                    double dt_seconds  = 0.2,    // simulation step
                    double tick_size   = 0.01);

    // Stop thread on destruction if still running (RAII safety).
    ~MarketSimulator();

    // Spawn worker thread and begin event loop.
    void start();
    // Signal loop to stop and join worker thread.
    void stop();

private:
    // Main loop: advance GBM, publish synthetic orders, respect running_ flag.
    void loop();
    // Helper to submit a framed limit order message into MatchingServer.
    void send_limit_order(uint8_t side, double price, uint64_t qty);

    MatchingServer* engine_;
    std::atomic<bool> running_;
    std::thread thread_;

    // GBM parameters and discretization cache
    // s_: last simulated price; mu_/sigma_: drift/vol (annualized); dt_: step seconds
    // tick_: price quantization; drift_term_/vol_term_: precomputed for efficiency
    double s_;
    double mu_;
    double sigma_;
    double dt_;
    double tick_;
    double drift_term_;
    double vol_term_;

    // RNG: mt19937_64 for reproducibility; normal for shocks; uniform for order sizes
    std::mt19937_64 rng_;
    std::normal_distribution<double> norm_;
    std::uniform_int_distribution<int> qty_dist_;
};

} // namespace quant
