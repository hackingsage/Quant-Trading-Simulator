// src/network_server.cpp
// Corrected, cross-platform, framed TCP server for the quant engine.
// - binds to 0.0.0.0 (LAN)
// - handles partial reads/writes
// - safe client removal
// - polls engine out_queue and broadcasts messages
//
// Note: depends on your existing headers for MatchingServer and ServerMessage.
// See messages.hpp / server.hpp / server.cpp for engine API. 
// Files used as references: messages.hpp, server.hpp, server.cpp. 
// :contentReference[oaicite:3]{index=3} :contentReference[oaicite:4]{index=4} :contentReference[oaicite:5]{index=5}

#include "quant/network_server.hpp"
#include "quant/server.hpp"
#include "quant/messages.hpp"

#include <vector>
#include <deque>
#include <unordered_map>
#include <iostream>
#include <cstring>
#include <cassert>
#include <chrono>
#include <algorithm>

#if defined(_WIN32) || defined(_WIN64)
  #define QPLAT_WINDOWS 1
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  #define CLOSESOCKET(s) closesocket(s)
  #define SOCKET_ERRNO() WSAGetLastError()
#else
  #define QPLAT_WINDOWS 0
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  using sock_t = int;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCKET(s) close(s)
  #define SOCKET_ERRNO() errno
#endif

namespace quant {

// Helper: set socket non-blocking
static bool set_nonblocking(sock_t s) {
#if QPLAT_WINDOWS
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) flags = 0;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// helper to write big-endian 32-bit length
static void write_u32_be(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((v >> 24) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 0) & 0xFF);
}

NetworkServer::NetworkServer(MatchingServer* engine, int port)
    : engine_(engine), port_(port), running_(false), listen_fd_(INVALID_SOCKET) {}

NetworkServer::~NetworkServer() {
    stop();
}

bool NetworkServer::start() {
    if (running_) return true;

#if QPLAT_WINDOWS
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        std::cerr << "[net] WSAStartup failed\n";
        return false;
    }
#endif

    // create listen socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd_ == INVALID_SOCKET) {
        std::cerr << "[net] socket() failed\n";
        return false;
    }

    // reuse addr
    int opt = 1;
#if QPLAT_WINDOWS
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = INADDR_ANY; // <-- bind to all interfaces (LAN)

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "[net] bind failed errno=" << SOCKET_ERRNO() << "\n";
        CLOSESOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_fd_, SOMAXCONN) != 0) {
        std::cerr << "[net] listen failed\n";
        CLOSESOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
        return false;
    }

    if (!set_nonblocking(listen_fd_)) {
        std::cerr << "[net] set_nonblocking(listen_fd) failed\n";
        // continue anyway
    }

    running_ = true;
    worker_thread_ = std::thread(&NetworkServer::run_loop, this);
    std::cout << "[net] listening on 0.0.0.0:" << port_ << "\n";
    return true;
}

void NetworkServer::stop() {
    if (!running_) return;
    running_ = false;
    if (worker_thread_.joinable()) worker_thread_.join();

    // close all client sockets
    for (auto &kv : clients_) {
        CLOSESOCKET(kv.second.fd);
    }
    clients_.clear();

    if (listen_fd_ != INVALID_SOCKET) {
        CLOSESOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
    }

#if QPLAT_WINDOWS
    WSACleanup();
#endif
}

void NetworkServer::run_loop() {
    // select() variables
    fd_set readset, writeset;
    const int maxfd_safe = FD_SETSIZE - 1;

    // main loop: accept clients, read/write sockets, and broadcast engine messages
    while (running_) {
        FD_ZERO(&readset);
        FD_ZERO(&writeset);

        // always monitor listen socket for new connections
        FD_SET((unsigned)listen_fd_, &readset);
        sock_t maxfd = listen_fd_;

        // set client fds
        for (auto &kv : clients_) {
            ClientState &cs = kv.second;
            FD_SET((unsigned)cs.fd, &readset);
            if (!cs.send_queue.empty()) {
                FD_SET((unsigned)cs.fd, &writeset);
            }
            if (cs.fd > maxfd) maxfd = cs.fd;
        }

        // set timeout to 100ms to poll engine messages frequently
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100 ms

        int nfds = (int)(maxfd + 1);
        int rc = select(nfds, &readset, &writeset, nullptr, &tv);
        if (rc < 0) {
            // transient errors should not crash server
            // On Windows, WSAGetLastError
            // On POSIX, errno
            // Sleep a bit and continue
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 1) Accept new clients (handle possibly multiple accepts)
        if (FD_ISSET((unsigned)listen_fd_, &readset)) {
            // accept in loop until EWOULDBLOCK
            while (true) {
                sockaddr_in cli_addr;
                socklen_t cli_len = sizeof(cli_addr);
                sock_t client_fd = accept(listen_fd_, (sockaddr*)&cli_addr, &cli_len);
                if (client_fd == INVALID_SOCKET) break;

                // set non-blocking
                set_nonblocking(client_fd);

                ClientState cs;
                cs.fd = client_fd;
                cs.recv_buffer.reserve(4096);
                cs.send_offset = 0;
                char ipbuf[64] = {0};
                inet_ntop(AF_INET, &cli_addr.sin_addr, ipbuf, sizeof(ipbuf));
                cs.peer = std::string(ipbuf) + ":" + std::to_string(ntohs(cli_addr.sin_port));

                clients_.emplace((int)client_fd, std::move(cs));
                std::cout << "[net] client connected: " << ipbuf << ":" << ntohs(cli_addr.sin_port) << "\n";
            }
        }

        // buffer clients to remove (avoid erasing while iterating)
        std::vector<int> to_remove;

        // 2) Read from clients that are readable
        for (auto &kv : clients_) {
            ClientState &cs = kv.second;
            if (!FD_ISSET((unsigned)cs.fd, &readset)) continue;

            // read available data into temporary buffer
            uint8_t tmp[4096];
            while (true) {
                int got = recv(cs.fd, (char*)tmp, sizeof(tmp), 0);
                if (got > 0) {
                    cs.recv_buffer.insert(cs.recv_buffer.end(), tmp, tmp + got);

                    // process as many complete frames as possible
                    while (true) {
                        if (cs.recv_buffer.size() < 4) break; // need header
                        // read length (big-endian)
                        uint32_t len = (uint32_t)cs.recv_buffer[0] << 24 |
                                       (uint32_t)cs.recv_buffer[1] << 16 |
                                       (uint32_t)cs.recv_buffer[2] << 8  |
                                       (uint32_t)cs.recv_buffer[3];
                        if (len > 10 * 1024 * 1024) {
                            // sanity: refuse absurd frame sizes
                            std::cerr << "[net] client " << cs.peer << " sent too large frame: " << len << "\n";
                            to_remove.push_back(kv.first);
                            break;
                        }
                        if (cs.recv_buffer.size() < 4 + len) break; // wait for full payload
                        // Extract payload
                        std::vector<uint8_t> payload;
                        payload.insert(payload.end(), cs.recv_buffer.begin() + 4, cs.recv_buffer.begin() + 4 + len);
                        // erase consumed bytes
                        cs.recv_buffer.erase(cs.recv_buffer.begin(), cs.recv_buffer.begin() + 4 + len);

                        // handle payload (client -> engine)
                        handle_client_payload(cs, payload);
                    } // while frames
                } else {
#if QPLAT_WINDOWS
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK || err == WSAEINTR) break;
#else
                    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) break;
#endif
                    // got <= 0 and not EWOULDBLOCK -> connection closed or error
                    to_remove.push_back(kv.first);
                    break;
                }
            } // while recv loop
        }

        // 3) Write to writable clients (handle partial writes)
        for (auto &kv : clients_) {
            ClientState &cs = kv.second;
            if (!FD_ISSET((unsigned)cs.fd, &writeset)) continue;
            while (!cs.send_queue.empty()) {
                std::vector<uint8_t> &front = cs.send_queue.front();
                const char* data = (const char*)front.data();
                size_t total = front.size();
                size_t offset = cs.send_offset;

                ssize_t sent = send(cs.fd, data + offset, static_cast<int>(total - offset), 0);
                if (sent > 0) {
                    cs.send_offset += (size_t)sent;
                    if (cs.send_offset >= total) {
                        // message fully sent
                        cs.send_queue.pop_front();
                        cs.send_offset = 0;
                        continue; // attempt next queued message
                    } else {
                        // partial write; try later
                        break;
                    }
                } else {
#if QPLAT_WINDOWS
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK || err == WSAEINTR) break;
#else
                    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) break;
#endif
                    // fatal send error
                    to_remove.push_back(kv.first);
                    break;
                }
            } // while send_queue
        }

        // 4) Broadcast engine messages (non-blocking: get all available and enqueue to clients)
        {
            ServerMessage sm;
            while (engine_->get_next_server_message(sm)) {
                std::vector<uint8_t> framed = pack_server_message(sm);
                // push to every client send_queue
                for (auto &kv : clients_) {
                    ClientState &cs = kv.second;
                    cs.send_queue.push_back(framed);
                }
            }
        }

        // 5) Remove and close clients outside iteration
        if (!to_remove.empty()) {
            // unique them
            std::sort(to_remove.begin(), to_remove.end());
            to_remove.erase(std::unique(to_remove.begin(), to_remove.end()), to_remove.end());
            for (int fdkey : to_remove) {
                auto it = clients_.find(fdkey);
                if (it != clients_.end()) {
                    std::cout << "[net] client disconnected: " << it->second.peer << "\n";
                    CLOSESOCKET(it->second.fd);
                    clients_.erase(it);
                }
            }
            to_remove.clear();
        }
    } // while running

    // final cleanup
    for (auto &kv : clients_) {
        CLOSESOCKET(kv.second.fd);
    }
    clients_.clear();

    if (listen_fd_ != INVALID_SOCKET) {
        CLOSESOCKET(listen_fd_);
        listen_fd_ = INVALID_SOCKET;
    }
}

// Called when a complete framed payload arrives from a client
void NetworkServer::handle_client_payload(ClientState &cs, const std::vector<uint8_t>& payload) {
    if (payload.empty()) return;
    uint8_t type = payload[0];

    // The payload layout must match your existing Node / client encoder.
    // We only support the same ClientMessage types you defined in messages.hpp: NEW_ORDER, CANCEL.
    if (type == static_cast<uint8_t>(NEW_ORDER)) {
        // expect: 1 byte type + 8 user_id + 1 side + 8 price(double) + 8 qty
        if (payload.size() < 1 + 8 + 1 + 8 + 8) {
            std::cerr << "[net] bad NEW_ORDER frame size from " << cs.peer << "\n";
            return;
        }
        size_t off = 1;
        uint64_t user_id = 0;
        for (int i = 0; i < 8; ++i) user_id = (user_id << 8) | payload[off + i];
        off += 8;
        uint8_t side = payload[off++];
        // read double BE
        double price = 0.0;
        uint64_t price_bits = 0;
        for (int i = 0; i < 8; ++i) price_bits = (price_bits << 8) | payload[off + i];
        off += 8;
        std::memcpy(&price, &price_bits, sizeof(price)); 
        // **Note**: system endianness may require byteswap for IEEE-754; we assume the encoding on client matches host.
        uint64_t qty = 0;
        for (int i = 0; i < 8; ++i) qty = (qty << 8) | payload[off + i];
        // build MsgNewOrder and push to engine
        MsgNewOrder m{};
        m.user_id = user_id;
        m.side = side;
        m.price = price;
        m.quantity = qty;
        engine_->submit_new_order(m);
    } else if (type == static_cast<uint8_t>(CANCEL)) {
        if (payload.size() < 1 + 8) {
            std::cerr << "[net] bad CANCEL frame size from " << cs.peer << "\n";
            return;
        }
        uint64_t order_id = 0;
        for (int i = 0; i < 8; ++i) order_id = (order_id << 8) | payload[1 + i];
        MsgCancel c{};
        c.order_id = order_id;
        engine_->submit_cancel(c);
    } else {
        // unknown client message; ignore or log
        std::cerr << "[net] unknown client message type=" << (int)type << " from " << cs.peer << "\n";
    }
}

// pack_server_message must return a framed message: 4-byte BE length + payload
std::vector<uint8_t> NetworkServer::pack_server_message(const ServerMessage& m) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(m.type));
    if (m.type == TRADE) {
        auto append_u64 = [&](uint64_t v) {
            for (int i = 7; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
        };
        auto append_double = [&](double x) {
            uint64_t v;
            std::memcpy(&v, &x, sizeof(v));
            for (int i = 7; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
        };
        append_u64(m.trade.trade_id);
        append_u64(m.trade.buy_order_id);
        append_u64(m.trade.buy_user_id);
        append_u64(m.trade.sell_order_id);
        append_u64(m.trade.sell_user_id);
        append_double(m.trade.price);
        append_u64(m.trade.quantity);

    }else if (m.type == ACK) {
        payload.push_back(m.ack.status);
        payload.push_back(m.ack.type);
        uint64_t v = m.ack.order_id;
        for (int i = 7; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
    } else if (m.type == TOB) {
        auto append_double = [&](double x) {
            uint64_t v;
            std::memcpy(&v, &x, sizeof(v));
            for (int i = 7; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
        };
        auto append_u64 = [&](uint64_t v) {
            for (int i = 7; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
        };
        append_double(m.tob.bid_price);
        append_u64(m.tob.bid_quantity);
        append_double(m.tob.ask_price);
        append_u64(m.tob.ask_quantity);
    } else if (m.type == L2_UPDATE) {
        payload.push_back(m.l2.side);
        uint64_t v;
        std::memcpy(&v, &m.l2.price, sizeof(v));
        for (int i = 7; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
        uint64_t q = m.l2.quantity;
        for (int i = 7; i >= 0; --i) payload.push_back((q >> (i*8)) & 0xFF);
    } else if (m.type == PNL_UPDATE) {
        auto append_double = [&](double x) {
            uint64_t v;
            std::memcpy(&v, &x, sizeof(v));
            for (int i = 7; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
        };
        auto append_u32 = [&](uint32_t v){
            for (int i = 3; i >= 0; --i) payload.push_back((v >> (i*8)) & 0xFF);
        };
        append_u32(m.pnl.user_id);
        append_double(m.pnl.realized);
        append_double(m.pnl.unrealized);
        append_double(m.pnl.position);
        append_double(m.pnl.avg_price);
        append_double(m.pnl.equity);
    }
    // frame it
    std::vector<uint8_t> framed;
    uint32_t len = static_cast<uint32_t>(payload.size());
    framed.reserve(4 + payload.size());
    write_u32_be(framed, len);
    framed.insert(framed.end(), payload.begin(), payload.end());
    return framed;
}

} // namespace quant
