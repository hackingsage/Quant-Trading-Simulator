g++ -std=c++17 -O3 -Iinclude src/order_book.cpp src/bs_bot.cpp src/server.cpp src/network_server.cpp src/market_sim.cpp src/pnl.cpp src/main.cpp -lws2_32 -o matching_server.exe
./matching_server