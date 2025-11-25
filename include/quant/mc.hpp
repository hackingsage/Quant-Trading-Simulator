#pragma once
#include <cstdint>
#include <cstddef>
#include <tuple>

namespace quant {
    // MCOptions: configuration for Monte Carlo pricing.
    // r is risk-free rate (annualized); seed controls RNG reproducibility; n_threads = 0 uses hardware_concurrency.
    struct MCOptions {
        std::size_t n_paths = 1000000;   // total number of Monte-Carlo paths
        std::size_t n_threads = 0;       // 0 => use hardware_concurrency
        bool use_antithetic = true;      // use antithetic variates
        bool use_control_variate = true; // use S_T as control variate
        uint64_t seed = 0;
        double r = 0.0;
    };

    // Result: estimated price, standard error, 95% CI [low,high], samples used (normal approximation)
    struct MCResult {
        double price;        
        double stderr_;       
        double ci_low;       
        double ci_high;      
        std::size_t n_samples;
    };

    // Monte Carlo function for European call/put with terminal-only payoff (vanilla).
    // is_call selects payoff: max(±(S_T - K), 0); risk-neutral drift uses r in opts.
    // If bs_price_fn != nullptr, it is used as a control variate baseline for variance reduction.
    MCResult monte_carlo_terminal(
        double S0,
        double K,
        double sigma,
        double T,
        const MCOptions& opts,
        bool is_call,
        double (*bs_price_fn)(double S, double K, double r, double sigma, double T) = nullptr
    );
}