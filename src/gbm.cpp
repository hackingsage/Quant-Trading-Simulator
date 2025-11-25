#include "quant/gbm.hpp"
#include <chrono>
#include <cmath>
using namespace std;

namespace quant{
    static uint64_t default_time_seed(){
        return static_cast<uint64_t>(
            chrono::high_resolution_clock::now().time_since_epoch().count()
        );
    }

    GBM::GBM(double S0, double mu, double sigma, uint64_t seed) 
        : S0_(S0), mu_(mu), sigma_(sigma), nd_(0.0, 1.0), seed_(seed){
            if(seed_ == 0) seed_ = default_time_seed();
            rng_.seed(seed_);
        }
    
    void GBM::reseed(uint64_t seed){
        seed_ = (seed == 0) ? default_time_seed() : seed;
        rng_.seed(seed_);
    }

    double GBM::sample_terminal(double T){
        double z = nd_(rng_);
        double drift = (mu_ - 0.5*sigma_*sigma_) * T;
        double vol = sigma_ * sqrt(T);
        return S0_ * exp(drift + vol * z);
    }

    vector<double> GBM::sample_path(double T, size_t n_steps){
        vector<double> path;
        path.reserve(n_steps + 1);
        path.push_back(S0_);
        if (n_steps = 0) return path;

        double dt = T / static_cast<double>(n_steps);
        double drift_dt = (mu_ - 0.5*sigma_*sigma_) * dt;
        double vol_sqrt_dt = sigma_ * sqrt(dt);

        double S = S0_;
        for(size_t i = 0; i < n_steps; i++){
            double z = nd_(rng_);
            S = S * exp(drift_dt + vol_sqrt_dt * z);
            path.push_back(S);
        }
        return path;
    }

    vector<double> GBM::sample_terminal_batch(size_t n_paths, double T){
        vector<double> out;
        out.reserve(n_paths);
        double drifts = (mu_ - 0.5*sigma_*sigma_) * T;
        double vol = sigma_ * sqrt(T);
        for(size_t i = 0; i < n_paths; i++){
            double z = nd_(rng_);
            out.push_back(S0_ * exp(drifts + vol * z));
        }
        return out;
    }
}