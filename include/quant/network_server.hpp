#pragma once

#include <thread>
#include <vector>
#include <deque>
#include <string>
#include <cstdint>
#include <atomic>
#include <unordered_map>

#if defined(_WIN32) || defined(_WIN64)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using qsocket_t = SOCKET;
#else
  #include <sys/socket.h>
  using qsocket_t = int;
#endif

namespace quant {

struct ServerMessage;
class MatchingServer;

/**
 * ClientState MUST be fully defined in the header,
 * because we store it by VALUE inside std::unordered_map.
 */
struct ClientState {
    qsocket_t fd;
    std::vector<uint8_t> recv_buffer;
    std::deque<std::vector<uint8_t>> send_queue;
    size_t send_offset = 0;
    std::string peer;
};

/**
 * NetworkServer:
 *  - Accepts multiple TCP clients
 *  - Receives framed messages
 *  - Passes orders to MatchingServer
 *  - Broadcasts engine messages
 *  - Handles partial reads/writes
 */
class NetworkServer {
public:
    explicit NetworkServer(MatchingServer* engine, int port);
    ~NetworkServer();

    // Bind/listen and spawn worker thread; returns false if already running or on bind/listen failure.
    bool start();
    // Stop accepting, close all client sockets, and join worker thread.
    void stop();

private:
    // Event loop: accept new clients, read frames, dispatch to engine, and broadcast engine messages.
    void run_loop();
    // Process a complete payload (after deframing) from a client; may enqueue orders/cancels.
    void handle_client_payload(ClientState &cs, const std::vector<uint8_t>& payload);

    // Frame a server message as [4-byte big-endian length][payload bytes].
    std::vector<uint8_t> pack_server_message(const ServerMessage& msg);

private:
    // Engine to bridge messages to/from.
    MatchingServer* engine_;
    // TCP port to bind.
    int port_;

    // Lifecycle state for worker loop.
    std::atomic<bool> running_;
    // Listening socket descriptor.
    qsocket_t listen_fd_;
    // Dedicated worker thread.
    std::thread worker_thread_;

    // Active clients keyed by fd; stores partial I/O state.
    std::unordered_map<int, ClientState> clients_;
};

} // namespace quant
