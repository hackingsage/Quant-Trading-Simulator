#include "quant/net_platform.hpp"
#include "quant/server.hpp"
#include "quant/network_server.hpp"
#include "quant/market_sim.hpp"
#include "quant/bs_bot.hpp"

#include <iostream>
#include <chrono>
#include <thread>

int main() {

#if QPLAT_WINDOWS
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    std::cout << "=== Starting Matching Engine ===\n";
    quant::MatchingServer engine(4096, 4096);
    engine.start();

    std::cout << "=== Starting Monte Carlo Market Simulator ===\n";
    quant::MarketSimulator sim(
        &engine,
        /*s0*/   100.0,
        /*mu*/   0.0,
        /*sigma*/0.20,
        /*dt*/   0.15,
        /*tick*/ 0.01
    );
    sim.start();

    std::cout << "=== Starting Black-Scholes Market-Making Bot ===\n";
    quant::BSBotConfig cfg;
    cfg.user_id = 9999;
    cfg.underlying_instrument = 1;
    cfg.option_instrument = 2;
    cfg.opt_type = quant::OptionType::Call;
    cfg.strike = 100.0;
    cfg.expiry_seconds = 3600.0 * 24;   // 1 day
    cfg.r = 0.00;
    cfg.iv = 0.20;
    cfg.spread = 0.5;                   // moderate spread
    cfg.qty = 5.0;
    cfg.hedge_tolerance = 0.5;

    quant::BSBot bot(&engine, cfg);
    bot.start();

    std::cout << "=== Starting TCP Network Server on port 9001 ===\n";
    quant::NetworkServer net(&engine, 9001);
    net.start();

    std::cout << "System ready. Press Ctrl+C to exit.\n";

    // Run indefinitely
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Cleanup (won't execute normally because of infinite loop)
    bot.stop();
    sim.stop();
    net.stop();
    engine.stop();

#if QPLAT_WINDOWS
    WSACleanup();
#endif

    return 0;
}
