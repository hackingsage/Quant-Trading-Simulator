#include "quant/mc.hpp"
#include "quant/gbm.hpp"
#include "quant/bs.hpp"
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <functional>
#include <iostream>

namespace quant {
    // Fallback seed source if user does not provide a seed.
    static uint64_t default_time_seed() {
        return static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
    }

    // Internal struct for per-thread accumulators (no allocations inside loops)
    // Per-thread accumulators to avoid contention and allocations in the hot path.
    struct ThreadAcc {
        // sums for payoff (Y) and control (X = S_T)
        long double sumY = 0.0L;
        long double sumY2 = 0.0L;
        long double sumX = 0.0L;
        long double sumX2 = 0.0L;
        long double sumYX = 0.0L;
        std::size_t n = 0;
    };

    // Vanilla terminal payoffs.
    static inline double payoff_call(double ST, double K) {
        return std::max(0.0, ST - K);
    }
    static inline double payoff_put(double ST, double K) {
        return std::max(0.0, K - ST);
    }

    MCResult monte_carlo_terminal(
        double S0,
        double K,
        double sigma,
        double T,
        const MCOptions& opts,
        bool is_call,
        double (*bs_price_fn)(double, double, double, double, double)
    ) {
        MCOptions local_opts = opts;
        // Resolve thread count and seed defaults.
        if (local_opts.n_threads == 0) {
            local_opts.n_threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        }
        if (local_opts.seed == 0) local_opts.seed = default_time_seed();

        const std::size_t n_total = local_opts.n_paths;
        const std::size_t n_threads = std::min(local_opts.n_threads, n_total);
        const bool use_antithetic = local_opts.use_antithetic;
        const bool use_cv = local_opts.use_control_variate;

        // Determine per-thread workloads. We'll split as evenly as possible.
        std::vector<std::size_t> counts(n_threads, n_total / n_threads);
        for (std::size_t i = 0; i < (n_total % n_threads); ++i) counts[i]++;

        // If antithetic, ensure each thread handles an even number of draws (pairs) to maintain pairing.
        if (use_antithetic) {
            for (std::size_t i = 0; i < n_threads; ++i) {
                if (counts[i] % 2 != 0) {
                    // borrow one from another thread if possible, else increment to next even
                    counts[i]++;
                    // adjust total counts if we exceed n_total -- that's ok; we'll report effective samples
                }
            }
        }

        // Container for per-thread accumulators
        std::vector<ThreadAcc> acc(n_threads);

        // We'll spawn threads and accumulate locally (no mutexes in hot loop).
        std::vector<std::thread> workers;
        workers.reserve(n_threads);

        // Prepare distinct per-thread seeds using seed_seq for reproducibility while avoiding correlation.
        std::vector<uint32_t> seq_input;
        seq_input.reserve(8);
        seq_input.push_back(static_cast<uint32_t>(local_opts.seed & 0xFFFFFFFFu)); //Lower 32 bit
        seq_input.push_back(static_cast<uint32_t>((local_opts.seed >> 32) & 0xFFFFFFFFu)); //Upper 32 bit
        std::seed_seq seedseq(seq_input.begin(), seq_input.end());
        std::vector<uint32_t> thread_seeds(n_threads);
        seedseq.generate(thread_seeds.begin(), thread_seeds.end());

        // Precompute drift/vol terms for terminal lognormal sampling; discount factor for price.
        const double drift = (local_opts.r - 0.5 * sigma * sigma) * T;
        const double vol = sigma * std::sqrt(T);
        const long double exp_neg_rT = std::expl(-local_opts.r * T);

        for (std::size_t t = 0; t < n_threads; ++t) {
            workers.emplace_back([t, &counts, &acc, S0, K, drift, vol, T, use_antithetic, use_cv, is_call, n_threads, &thread_seeds, local_opts]() {
                // each thread local rng
                // Per-thread RNG; seed mixed with index to decorrelate streams.
                std::mt19937_64 rng;
                // Derive a unique seed for each thread based on thread_seeds and thread index
                // Combine with index to ensure distinctness
                // 0x9e3779b97f4a7c15ULL Golden ratio constant for good mixing
                uint64_t thread_seed = static_cast<uint64_t>(thread_seeds[t]) ^ (uint64_t(0x9e3779b97f4a7c15ULL) + (uint64_t)t + (static_cast<uint64_t>(thread_seeds[0])<<1));
                rng.seed(thread_seed);

                std::normal_distribution<double> nd(0.0, 1.0);

                ThreadAcc local;
                const std::size_t my_count = counts[t];

                // Hot loop: generate samples and accumulate sums. No heap allocs here.
                std::size_t i = 0;
                if (use_antithetic) {
                    // Antithetic variates: use z and -z to reduce variance of the estimator.
                    // process in pairs
                    for (; i + 1 < my_count; i += 2) {
                        double z = nd(rng);
                        double z2 = -z;

                        double ST1 = S0 * std::exp(drift + vol * z);
                        double ST2 = S0 * std::exp(drift + vol * z2);

                        double Y1 = is_call ? payoff_call(ST1, K) : payoff_put(ST1, K);
                        double Y2 = is_call ? payoff_call(ST2, K) : payoff_put(ST2, K);

                        // control variate X = ST (undiscounted)
                        double X1 = ST1;
                        double X2 = ST2;

                        local.sumY  += static_cast<long double>(Y1 + Y2);
                        local.sumY2 += static_cast<long double>(Y1*Y1 + Y2*Y2);
                        local.sumX  += static_cast<long double>(X1 + X2);
                        local.sumX2 += static_cast<long double>(X1*X1 + X2*X2);
                        local.sumYX += static_cast<long double>(Y1*X1 + Y2*X2);
                        local.n += 2;
                    }
                    // If my_count was odd, do one more
                    if (i < my_count) {
                        double z = nd(rng);
                        double ST = S0 * std::exp(drift + vol * z);
                        double Y = is_call ? payoff_call(ST, K) : payoff_put(ST, K);
                        double X = ST;
                        local.sumY  += static_cast<long double>(Y);
                        local.sumY2 += static_cast<long double>(Y*Y);
                        local.sumX  += static_cast<long double>(X);
                        local.sumX2 += static_cast<long double>(X*X);
                        local.sumYX += static_cast<long double>(Y*X);
                        local.n += 1;
                    }
                } else {
                    for (; i < my_count; ++i) {
                        double z = nd(rng);
                        double ST = S0 * std::exp(drift + vol * z);
                        double Y = is_call ? payoff_call(ST, K) : payoff_put(ST, K);
                        double X = ST;
                        local.sumY  += static_cast<long double>(Y);
                        local.sumY2 += static_cast<long double>(Y*Y);
                        local.sumX  += static_cast<long double>(X);
                        local.sumX2 += static_cast<long double>(X*X);
                        local.sumYX += static_cast<long double>(Y*X);
                        local.n += 1;
                    }
                }
                // store local results
                acc[t] = local;
            });
        }

        // join threads
        for (auto &th : workers) th.join();

        // combine results from threads
        long double sumY = 0.0L, sumY2 = 0.0L, sumX = 0.0L, sumX2 = 0.0L, sumYX = 0.0L;
        std::size_t N = 0;
        for (std::size_t t = 0; t < n_threads; ++t) {
            sumY  += acc[t].sumY;
            sumY2 += acc[t].sumY2;
            sumX  += acc[t].sumX;
            sumX2 += acc[t].sumX2;
            sumYX += acc[t].sumYX;
            N += acc[t].n;
        }

        if (N == 0) {
            return MCResult{0.0, 0.0, 0.0, 0.0, 0};
        }

        // Sample means on undiscounted payoffs (Y) and control variate (X = S_T).
        long double meanY = sumY / static_cast<long double>(N);
        long double meanX = sumX / static_cast<long double>(N);

        // Control variate expectation under risk-neutral measure: E[S_T] = S0 * exp(r T).
        long double EX = static_cast<long double>(S0 * std::exp(local_opts.r * T));

        // Covariance and variance (unbiased, N-1) for optimal control variate coefficient.
        long double covYX = (sumYX - static_cast<long double>(N) * meanY * meanX) / static_cast<long double>(N - 1);
        long double varX  =  (sumX2 - static_cast<long double>(N) * meanX * meanX) / static_cast<long double>(N - 1);
        long double varY  =  (sumY2 - static_cast<long double>(N) * meanY * meanY) / static_cast<long double>(N - 1);

        long double b_opt = 0.0L;
        if (use_cv && varX > 0.0L) {
            b_opt = covYX / varX;
        }

        // Construct adjusted estimator (undiscounted) with optimal b: Y_cv = Y - b*(X - E[X]).
        // Y_cv = Y - b*(X - E[X])
        // mean(Y_cv) = meanY - b*(meanX - EX)
        long double meanYcv = meanY - b_opt * (meanX - EX);

        // Variance of adjusted estimator: var(Y - bX) = varY - 2 b covYX + b^2 varX; clamp at 0 for safety.
        long double varYcv = varY - 2.0L * b_opt * covYX + b_opt * b_opt * varX;
        if (varYcv < 0.0L) varYcv = 0.0L; // numerical safety

        // Discount to present value.
        long double price = exp_neg_rT * meanYcv;

        long double stderr_ = 0.0L;
        if (N > 0) {
            long double var_mean = varYcv / static_cast<long double>(N);
            stderr_ = static_cast<double>(std::sqrt(var_mean) * exp_neg_rT);
        }

        // Symmetric 95% normal-approximation confidence interval.
        const long double z95 = 1.959963984540054; // ~1.96
        long double ci_low  = price - z95 * stderr_;
        long double ci_high = price + z95 * stderr_;

        MCResult res;
        res.price = static_cast<double>(price);
        res.stderr_ = static_cast<double>(stderr_);
        res.ci_low = static_cast<double>(ci_low);
        res.ci_high = static_cast<double>(ci_high);
        res.n_samples = N;
        return res;
    }
}