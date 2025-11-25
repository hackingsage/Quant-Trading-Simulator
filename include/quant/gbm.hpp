#pragma once
#include <random>
#include <cstdint>
using namespace std;

namespace quant{
    // GBM
    //
    // Geometric Brownian Motion process with SDE:
    //   dS = mu * S * dt + sigma * S * dW
    // Discretized via log-Euler to preserve positivity. Exposes terminal
    // sampling, full path generation, and RNG reseeding for reproducibility.
    class GBM{
        public:
            GBM(double S0, double mu, double sigma, uint64_t seed = 0);
            // Sample a single terminal price S_T for maturity T (years) using lognormal step.
            double sample_terminal(double T);
            // Sample a single path with n_steps time steps over [0, T]; log-Euler ensures S>0.
            vector<double> sample_path(double T, size_t n_steps);
            // Sample n_paths terminal prices for maturity T; useful for Monte Carlo.
            vector<double> sample_terminal_batch(size_t n_paths, double T);
            // Reset RNG seed to reproduce sequences across runs.
            void reseed(uint64_t seed);
        private:
            double S0_;
            double mu_;
            double sigma_;
            uint64_t seed_;
            normal_distribution<double> nd_;
            mt19937_64 rng_;
    };
}