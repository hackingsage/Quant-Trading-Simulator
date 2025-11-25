#include "quant/market_sim.hpp"
#include <chrono>
#include <cmath>

namespace quant {

MarketSimulator::MarketSimulator(MatchingServer* engine,
                                 double s0,
                                 double mu,
                                 double sigma,
                                 double dt_seconds,
                                 double tick_size)
    : engine_(engine),
      running_(false),
      s_(s0),
      mu_(mu),
      sigma_(sigma),
      dt_(dt_seconds),
      tick_(tick_size),
      rng_(std::random_device{}()),
      norm_(0.0, 1.0),
      qty_dist_(1, 20)
{
    // Precompute drift & vol terms for GBM (kept for compatibility,
    // but we now use a mean-reverting log process in loop()).
    drift_term_ = (mu_ - 0.5 * sigma_ * sigma_) * dt_;
    vol_term_   = sigma_ * std::sqrt(dt_);
}

MarketSimulator::~MarketSimulator() {
    stop();
}

void MarketSimulator::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&MarketSimulator::loop, this);
}

void MarketSimulator::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void MarketSimulator::send_limit_order(uint8_t side, double price, uint64_t qty) {
    MsgNewOrder m{};
    m.user_id  = 0;      // simulated market user id
    m.side     = side;     // 0 = buy, 1 = sell
    m.price    = price;
    m.quantity = qty;
    engine_->submit_new_order(m);
}

void MarketSimulator::loop() {
    using namespace std::chrono;

    // Mean-reversion target around the original spot
    const double mean_level = 100.0;   // you can tweak this
    const double kappa      = 1.0;     // mean reversion speed

    while (running_) {
        // 1) advance a mean-reverting *log-price* process
        double z = norm_(rng_);

        // work in log space
        double logS     = std::log(std::max(s_, tick_));   // avoid log(0)
        double logMean  = std::log(mean_level);

        // Ornstein–Uhlenbeck on log S:
        // d logS = kappa (logMean - logS) dt + sigma dW
        logS += kappa * (logMean - logS) * dt_ + sigma_ * std::sqrt(dt_) * z;

        s_ = std::exp(logS);

        // 2) round to ticks
        auto round_to_tick = [&](double x) {
            double ticks = std::round(x / tick_);
            return ticks * tick_;
        };
        double mid = round_to_tick(s_);

        // Make sure mid is sensible
        if (mid <= 0.0) mid = tick_;

        // 3) create some passive depth around mid
        double passive_bid = round_to_tick(mid - 0.5);
        double passive_ask = round_to_tick(mid + 0.5);

        if (passive_bid > 0.0) {
            send_limit_order(/*buy*/ 0, passive_bid, (uint64_t)qty_dist_(rng_));
        }
        send_limit_order(/*sell*/ 1, passive_ask, (uint64_t)qty_dist_(rng_));

        // 4) generate a crossing pair near mid to create trades
        double aggressive_bid = round_to_tick(mid + 0.05);
        double aggressive_ask = round_to_tick(mid - 0.05);
        if (aggressive_ask < aggressive_bid) {
            uint64_t q = (uint64_t)qty_dist_(rng_);
            // send buy first, then sell so they cross and trade
            send_limit_order(0, aggressive_bid, q);
            send_limit_order(1, aggressive_ask, q);
        }

        // 5) sleep until next step
        std::this_thread::sleep_for(duration<double>(dt_));
    }
}

} // namespace quant
