# Quant-Trading-Simulator

This repository contains a C++ quantitative trading simulator featuring a high-performance matching engine, a synthetic market data generator, and an automated Black-Scholes market-making bot. The system is visualized through a React-based web interface, connected via a Node.js WebSocket bridge.

## Architecture

The simulator is composed of three main components that run concurrently:

1.  **C++ Backend (`matching_server.exe`)**: The core of the system, built for performance.
    *   **`MatchingServer`**: Orchestrates message flow between components using lock-free SPSC queues.
    *   **`OrderBook`**: An in-memory, price-time priority limit order book for matching buy and sell orders.
    *   **`MarketSimulator`**: Generates a realistic, synthetic order flow by modeling the mid-price with a mean-reverting process based on Geometric Brownian Motion (GBM). It creates passive depth and crossing orders to simulate trades.
    *   **`BSBot`**: An automated market-making bot that quotes two-sided markets for an option contract. It calculates the theoretical price using the Black-Scholes model and actively hedges its delta exposure in the underlying instrument.
    *   **`PnLEngine`**: Tracks realized and unrealized Profit and Loss (PnL), position, and equity for multiple users, including the manual trader and the automated bot.
    *   **`NetworkServer`**: A non-blocking, multi-client TCP server running on port `9001` that communicates with the bridge using a custom binary protocol (length-prefixed frames).

2.  **Node.js Bridge (`bridge/`)**: A crucial link between the C++ backend and the web UI.
    *   Connects to the C++ backend's TCP server on port `9001`.
    *   Listens for WebSocket connections from the web UI on port `8080`.
    *   Decodes binary data from the C++ backend and broadcasts it as JSON messages to all connected web clients.
    *   Encodes JSON messages (new orders, cancels) from the web UI into binary frames and forwards them to the C++ backend.

3.  **React Web UI (`web-ui/`)**: A real-time dashboard for visualization and interaction.
    *   Connects to the WebSocket bridge to receive live market data.
    *   Displays the Level 2 order book, recent trades, and a live price chart.
    *   Features a trade ticket for submitting manual buy/sell orders.
    *   Shows separate, real-time PnL dashboards for the manual user and the BS bot.

## Features

-   High-performance C++ matching engine with price-time priority.
-   Synthetic market data generation using a mean-reverting stochastic process.
-   Automated market-making bot based on the Black-Scholes model with delta hedging.
-   Real-time, multi-user PnL tracking (realized, unrealized, position, and equity).
-   Low-latency communication via a TCP server and a custom binary protocol.
-   WebSocket bridge to make backend data accessible to a modern web frontend.
-   Interactive React dashboard for live data visualization and manual trading.

## How to Run

### Prerequisites

-   A C++17 compliant compiler (e.g., g++).
-   Node.js and npm.
-   For Windows, you will need the Winsock library (`ws2_32`).

### Step 1: Run the C++ Backend

The C++ executable bundles the matching engine, market simulator, BS bot, and TCP server.

1.  Navigate to the root directory of the repository.
2.  Compile the source files. The `run.sh` script provides the command. On Windows, you must link the Winsock library.

    ```bash
    # On Linux/macOS or Git Bash on Windows
    g++ -std=c++17 -O3 -Iinclude src/order_book.cpp src/bs_bot.cpp src/server.cpp src/network_server.cpp src/market_sim.cpp src/pnl.cpp src/main.cpp -lws2_32 -o matching_server.exe
    ```
    *Note: The `-lws2_32` flag is for Windows linkers (like MinGW g++).*

3.  Run the compiled server.

    ```bash
    ./matching_server.exe
    ```

    The backend is now running and the TCP server is listening on `127.0.0.1:9001`.

### Step 2: Run the Node.js Bridge

The bridge connects the C++ backend to the web UI.

1.  Open a new terminal and navigate to the `bridge` directory.
    ```bash
    cd bridge
    ```
2.  Install the dependencies.
    ```bash
    npm install
    ```
3.  Start the bridge.
    ```bash
    npm start
    ```
    The bridge is now running and the WebSocket server is listening on port `8080`.

### Step 3: Run the React Web UI

The UI provides the visualization dashboard.

1.  Open a third terminal and navigate to the `web-ui` directory.
    ```bash
    cd web-ui
    ```
2.  Install the dependencies.
    ```bash
    npm install
    ```
3.  Start the Vite development server.
    ```bash
    npm run dev
    ```
4.  Open your web browser and navigate to the local URL shown in the terminal (e.g., `http://localhost:5173`). You should see the live trading dashboard.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for more information.