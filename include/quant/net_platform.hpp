#pragma once

#include <cstdint>

#if defined(_WIN32) || defined(_WIN64)
    #define QPLAT_WINDOWS 1
#else
    #define QPLAT_WINDOWS 0
#endif

#if QPLAT_WINDOWS

// ---------------------- WINDOWS ----------------------
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

using qsocket_t = SOCKET;
static const qsocket_t QINVALID_SOCKET = INVALID_SOCKET;

inline bool q_set_nonblocking(qsocket_t s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

inline void q_close(qsocket_t s) {
    closesocket(s);
}

inline int q_last_error() {
    return WSAGetLastError();
}

static const int QERR_WOULDBLOCK = WSAEWOULDBLOCK;

#else

// ---------------------- LINUX / UNIX ----------------------
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

using qsocket_t = int;
static const qsocket_t QINVALID_SOCKET = -1;

inline bool q_set_nonblocking(qsocket_t s) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) != -1;
}

inline void q_close(qsocket_t s) {
    close(s);
}

inline int q_last_error() {
    return errno;
}

static const int QERR_WOULDBLOCK = EWOULDBLOCK;

#endif
