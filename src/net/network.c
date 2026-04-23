#include "network.h"
#include "mp_log.h"

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    pragma comment(lib, "ws2_32.lib")
#    define INVALID_SOCK        INVALID_SOCKET
#    define SOCK_ERR            SOCKET_ERROR
#    define closesock           closesocket
#    define sock_errno()        WSAGetLastError()
#    define WOULD_BLOCK(e)      ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
typedef SOCKET sock_t;
typedef int socklen_t;
#else
#    include <sys/socket.h>
#    include <sys/select.h>
#    include <netinet/in.h>
#    include <arpa/inet.h>
#    include <unistd.h>
#    include <fcntl.h>
#    include <errno.h>
#    define INVALID_SOCK        (-1)
#    define SOCK_ERR            (-1)
#    define closesock           close
#    define sock_errno()        errno
#    define WOULD_BLOCK(e)      ((e) == EINPROGRESS || (e) == EWOULDBLOCK)
typedef int sock_t;
#endif

// ---------------------------------------------------------------------------
// Per-client slot
// ---------------------------------------------------------------------------

#define RECV_BUF_SIZE (NET_MAX_MESSAGE_SIZE * 4)

typedef struct {
    sock_t   sock;
    bool     connected;
    time_t   last_recv;
    uint8_t  recv_buf[RECV_BUF_SIZE];
    int      recv_buf_len;
} ClientSlot;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static sock_t       server_sock  = INVALID_SOCK;
static sock_t       client_sock  = INVALID_SOCK; // used when we are a client
static ClientSlot   clients[NET_MAX_CLIENTS];
static int          client_count = 0;
static bool         is_host      = false;
static bool         is_active    = false;
static bool         wsa_init     = false;

static NetMessageHandler message_handler = NULL;
static NetEventHandler   event_handler   = NULL;

// Configurable globals (written by UI before connecting)
unsigned int g_server_options  = 0;
char         g_mp_join_address[256] = "127.0.0.1";
int          g_mp_max_players  = 8;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Send all bytes, looping on partial sends. Returns false on error.
static bool send_all(sock_t s, const uint8_t* data, int size)
{
    int sent = 0;
    while (sent < size) {
        int r = send(s, (const char*)data + sent, size - sent, 0);
        if (r <= 0) {
            return false;
        }
        sent += r;
    }
    return true;
}

static void disconnect_client(int slot)
{
    if (!clients[slot].connected) {
        return;
    }
    MP_WARN(MP_CAT_NET, "Disconnecting client slot %d", slot);
    closesock(clients[slot].sock);
    clients[slot].sock      = INVALID_SOCK;
    clients[slot].connected = false;
    clients[slot].recv_buf_len = 0;
    client_count--;
    if (event_handler) {
        event_handler(NET_EVENT_CLIENT_DISCONNECTED, slot);
    }
}

// Dispatch all complete framed messages sitting in slot's recv buffer.
static void drain_recv_buf(int slot)
{
    ClientSlot* cs = &clients[slot];
    while (cs->recv_buf_len >= 4) {
        uint32_t msg_len = *(uint32_t*)cs->recv_buf;
        if (msg_len == 0 || msg_len > (uint32_t)NET_MAX_MESSAGE_SIZE) {
            MP_ERROR(MP_CAT_NET, "Framing error from client %d: bad length %u — disconnecting", slot, msg_len);
            disconnect_client(slot);
            return;
        }
        if (cs->recv_buf_len < (int)(4 + msg_len)) {
            break; // Incomplete message — wait for more data
        }
        // Complete message available
        uint8_t* msg = cs->recv_buf + 4;
        MP_LOG_RECV(*(int*)msg, (int)msg_len);
        if (message_handler) {
            message_handler(msg);
        }
        int total = 4 + (int)msg_len;
        memmove(cs->recv_buf, cs->recv_buf + total, cs->recv_buf_len - total);
        cs->recv_buf_len -= total;
    }
}

// Same drain logic for the client socket (single connection to host).
// Uses a static buffer since there's only one client socket.
static uint8_t s_client_recv_buf[RECV_BUF_SIZE];
static int     s_client_recv_len = 0;

static void drain_client_recv_buf(void)
{
    while (s_client_recv_len >= 4) {
        uint32_t msg_len = *(uint32_t*)s_client_recv_buf;
        if (msg_len == 0 || msg_len > (uint32_t)NET_MAX_MESSAGE_SIZE) {
            MP_ERROR(MP_CAT_NET, "Framing error from server: bad length %u — dropping connection", msg_len);
            closesock(client_sock);
            client_sock = INVALID_SOCK;
            is_active   = false;
            s_client_recv_len = 0;
            if (event_handler) {
                event_handler(NET_EVENT_CONNECTION_LOST, 0);
            }
            return;
        }
        if (s_client_recv_len < (int)(4 + msg_len)) {
            break;
        }
        uint8_t* msg = s_client_recv_buf + 4;
        MP_LOG_RECV(*(int*)msg, (int)msg_len);
        if (message_handler) {
            message_handler(msg);
        }
        int total = 4 + (int)msg_len;
        memmove(s_client_recv_buf, s_client_recv_buf + total, s_client_recv_len - total);
        s_client_recv_len -= total;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool net_init(void)
{
    if (wsa_init) {
        return true;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MP_ERROR(MP_CAT_NET, "WSAStartup failed (%d)", WSAGetLastError());
        return false;
    }
#endif
    wsa_init = true;
    MP_DEBUG(MP_CAT_NET, "Network initialized");
    return true;
}

void net_cleanup(void)
{
    if (is_host) {
        net_stop_server();
    } else if (is_active) {
        net_stop_client();
    }
#ifdef _WIN32
    if (wsa_init) {
        WSACleanup();
    }
#endif
    wsa_init = false;
    MP_DEBUG(MP_CAT_NET, "Network cleaned up");
}

bool net_start_server(void)
{
    struct sockaddr_in addr;

    if (!net_init()) {
        return false;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock == INVALID_SOCK) {
        MP_ERROR(MP_CAT_NET, "socket() failed (%d)", sock_errno());
        return false;
    }

    // Allow address reuse so we can restart quickly after a crash
    int reuse = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    set_nonblocking(server_sock);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Accept LAN + localhost
    addr.sin_port        = htons(NET_PORT);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCK_ERR) {
        MP_ERROR(MP_CAT_NET, "bind() failed on port %d (%d)", NET_PORT, sock_errno());
        closesock(server_sock);
        server_sock = INVALID_SOCK;
        return false;
    }

    if (listen(server_sock, NET_MAX_CLIENTS) == SOCK_ERR) {
        MP_ERROR(MP_CAT_NET, "listen() failed (%d)", sock_errno());
        closesock(server_sock);
        server_sock = INVALID_SOCK;
        return false;
    }

    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        clients[i].sock = INVALID_SOCK;
    }
    client_count = 0;
    is_host      = true;
    is_active    = true;

    MP_INFO(MP_CAT_NET, "Server listening on 0.0.0.0:%d (max clients: %d)",
            NET_PORT, g_mp_max_players);
    return true;
}

void net_stop_server(void)
{
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (clients[i].connected) {
            closesock(clients[i].sock);
            clients[i].sock      = INVALID_SOCK;
            clients[i].connected = false;
        }
    }
    if (server_sock != INVALID_SOCK) {
        closesock(server_sock);
        server_sock = INVALID_SOCK;
    }
    client_count = 0;
    is_host      = false;
    is_active    = false;
    MP_INFO(MP_CAT_NET, "Server stopped");
}

bool net_start_client(const char* host)
{
    struct sockaddr_in addr;
    const char* target = (host && *host) ? host : "127.0.0.1";

    if (!net_init()) {
        return false;
    }

    client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_sock == INVALID_SOCK) {
        MP_ERROR(MP_CAT_NET, "socket() failed (%d)", sock_errno());
        return false;
    }

    set_nonblocking(client_sock);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(target);
    addr.sin_port        = htons(NET_PORT);

    MP_INFO(MP_CAT_NET, "Connecting to %s:%d ...", target, NET_PORT);

    int r = connect(client_sock, (struct sockaddr*)&addr, sizeof(addr));
    if (r == SOCK_ERR) {
        int e = sock_errno();
        if (!WOULD_BLOCK(e)) {
            MP_ERROR(MP_CAT_NET, "connect() failed (%d)", e);
            closesock(client_sock);
            client_sock = INVALID_SOCK;
            return false;
        }
        // EINPROGRESS / WSAEWOULDBLOCK is expected for non-blocking connect
    }

    s_client_recv_len = 0;
    is_host   = false;
    is_active = true;

    MP_INFO(MP_CAT_NET, "Connection initiated to %s:%d", target, NET_PORT);
    return true;
}

void net_stop_client(void)
{
    if (client_sock != INVALID_SOCK) {
        closesock(client_sock);
        client_sock = INVALID_SOCK;
    }
    s_client_recv_len = 0;
    is_active = false;
    MP_INFO(MP_CAT_NET, "Client stopped");
}

bool net_is_active(void)
{
    return is_active;
}

bool net_is_host(void)
{
    return is_host;
}

bool net_client_is_connected(int client_id)
{
    if (client_id < 0 || client_id >= NET_MAX_CLIENTS) {
        return false;
    }
    return clients[client_id].connected;
}

int net_client_count(void)
{
    return client_count;
}

unsigned int net_get_server_options(void)
{
    return g_server_options;
}

void net_set_server_options(unsigned int opts)
{
    g_server_options = opts;
    MP_DEBUG(MP_CAT_NET, "Server options set: 0x%04X", opts);
}

// Send to all connected clients (host) or to server (client).
void net_send_message(void* msg, int size)
{
    if (!is_active || size <= 0 || size > NET_MAX_MESSAGE_SIZE) {
        return;
    }

    // Build framed packet: [uint32 length][data]
    uint8_t frame[NET_MAX_MESSAGE_SIZE + 4];
    *(uint32_t*)frame = (uint32_t)size;
    memcpy(frame + 4, msg, size);

    MP_LOG_SEND(*(int*)msg, size);

    if (is_host) {
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (!clients[i].connected) {
                continue;
            }
            if (!send_all(clients[i].sock, frame, size + 4)) {
                MP_WARN(MP_CAT_NET, "Send to client %d failed — disconnecting", i);
                disconnect_client(i);
            }
        }
    } else {
        if (client_sock != INVALID_SOCK) {
            if (!send_all(client_sock, frame, size + 4)) {
                MP_WARN(MP_CAT_NET, "Send to server failed — connection lost");
                closesock(client_sock);
                client_sock = INVALID_SOCK;
                is_active   = false;
                if (event_handler) {
                    event_handler(NET_EVENT_CONNECTION_LOST, 0);
                }
            }
        }
    }
}

// Send to a specific client slot only (host → one client).
void net_send_message_to(int client_id, void* msg, int size)
{
    if (!is_host || client_id < 0 || client_id >= NET_MAX_CLIENTS) {
        return;
    }
    if (!clients[client_id].connected || size <= 0 || size > NET_MAX_MESSAGE_SIZE) {
        return;
    }

    uint8_t frame[NET_MAX_MESSAGE_SIZE + 4];
    *(uint32_t*)frame = (uint32_t)size;
    memcpy(frame + 4, msg, size);

    MP_LOG_SEND(*(int*)msg, size);

    if (!send_all(clients[client_id].sock, frame, size + 4)) {
        MP_WARN(MP_CAT_NET, "send_to client %d failed — disconnecting", client_id);
        disconnect_client(client_id);
    }
}

// Broadcast to all clients except one (host only).
void net_send_message_except(int except_client_id, void* msg, int size)
{
    if (!is_host || size <= 0 || size > NET_MAX_MESSAGE_SIZE) {
        return;
    }

    uint8_t frame[NET_MAX_MESSAGE_SIZE + 4];
    *(uint32_t*)frame = (uint32_t)size;
    memcpy(frame + 4, msg, size);

    MP_LOG_SEND(*(int*)msg, size);

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (i == except_client_id || !clients[i].connected) {
            continue;
        }
        if (!send_all(clients[i].sock, frame, size + 4)) {
            MP_WARN(MP_CAT_NET, "send_except: client %d failed — disconnecting", i);
            disconnect_client(i);
        }
    }
}

void net_set_message_handler(NetMessageHandler handler)
{
    message_handler = handler;
}

void net_set_event_handler(NetEventHandler handler)
{
    event_handler = handler;
}

void net_poll(void)
{
    if (!is_active) {
        return;
    }

    if (is_host) {
        // Build fd_set for server listen socket + all client sockets
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(server_sock, &read_set);
        sock_t max_fd = server_sock;

        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (clients[i].connected) {
                FD_SET(clients[i].sock, &read_set);
                if (clients[i].sock > max_fd) {
                    max_fd = clients[i].sock;
                }
            }
        }

        struct timeval timeout = { 0, 0 }; // Non-blocking poll
        int activity = select((int)max_fd + 1, &read_set, NULL, NULL, &timeout);
        if (activity <= 0) {
            return;
        }

        // Accept new connections
        if (FD_ISSET(server_sock, &read_set)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            sock_t new_sock = accept(server_sock,
                                     (struct sockaddr*)&client_addr, &addr_len);
            if (new_sock != INVALID_SOCK) {
                if (client_count >= g_mp_max_players) {
                    MP_WARN(MP_CAT_NET, "Rejecting connection: max players (%d) reached",
                            g_mp_max_players);
                    closesock(new_sock);
                } else {
                    // Find empty slot
                    int slot = -1;
                    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
                        if (!clients[i].connected) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot == -1) {
                        // Shouldn't happen if client_count tracks correctly
                        MP_ERROR(MP_CAT_NET, "No free client slot despite count OK");
                        closesock(new_sock);
                    } else {
                        set_nonblocking(new_sock);
                        clients[slot].sock         = new_sock;
                        clients[slot].connected    = true;
                        clients[slot].last_recv    = time(NULL);
                        clients[slot].recv_buf_len = 0;
                        client_count++;
                        MP_INFO(MP_CAT_NET, "Client connected: slot=%d addr=%s port=%d",
                                slot,
                                inet_ntoa(client_addr.sin_addr),
                                ntohs(client_addr.sin_port));
                        if (event_handler) {
                            event_handler(NET_EVENT_CLIENT_CONNECTED, slot);
                        }
                    }
                }
            }
        }

        // Receive from each connected client
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (!clients[i].connected || !FD_ISSET(clients[i].sock, &read_set)) {
                continue;
            }

            ClientSlot* cs  = &clients[i];
            int space       = RECV_BUF_SIZE - cs->recv_buf_len;
            int r           = recv(cs->sock,
                                   (char*)cs->recv_buf + cs->recv_buf_len,
                                   space, 0);
            if (r > 0) {
                cs->recv_buf_len += r;
                cs->last_recv = time(NULL);
                drain_recv_buf(i);
            } else if (r == 0) {
                MP_WARN(MP_CAT_NET, "Client %d closed connection gracefully", i);
                disconnect_client(i);
            } else {
                int e = sock_errno();
                if (!WOULD_BLOCK(e)) {
                    MP_WARN(MP_CAT_NET, "recv from client %d error %d — disconnecting", i, e);
                    disconnect_client(i);
                }
            }
        }

        // Timeout detection: 30 s with no data
        time_t now = time(NULL);
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (clients[i].connected &&
                (now - clients[i].last_recv) > 30) {
                MP_WARN(MP_CAT_NET, "Client %d timed out (30 s silence)", i);
                disconnect_client(i);
            }
        }

    } else {
        // We are a client — poll our single connection to the server
        fd_set read_set, write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(client_sock, &read_set);
        FD_SET(client_sock, &write_set); // Used to detect async connect completion

        struct timeval timeout = { 0, 0 };
        int activity = select((int)client_sock + 1,
                              &read_set, &write_set, NULL, &timeout);
        if (activity <= 0) {
            return;
        }

        // If writable and not yet fully connected, check for connect error
        if (FD_ISSET(client_sock, &write_set)) {
            int err = 0;
            socklen_t errlen = sizeof(err);
            getsockopt(client_sock, SOL_SOCKET, SO_ERROR,
                       (char*)&err, &errlen);
            if (err != 0) {
                MP_ERROR(MP_CAT_NET, "Async connect failed: error %d", err);
                closesock(client_sock);
                client_sock = INVALID_SOCK;
                is_active   = false;
                if (event_handler) {
                    event_handler(NET_EVENT_CONNECTION_LOST, 0);
                }
                return;
            }
            // else: connected — writable just means the socket is ready
        }

        if (FD_ISSET(client_sock, &read_set)) {
            int space = RECV_BUF_SIZE - s_client_recv_len;
            int r = recv(client_sock,
                         (char*)s_client_recv_buf + s_client_recv_len,
                         space, 0);
            if (r > 0) {
                s_client_recv_len += r;
                drain_client_recv_buf();
            } else if (r == 0) {
                MP_WARN(MP_CAT_NET, "Server closed connection");
                closesock(client_sock);
                client_sock = INVALID_SOCK;
                is_active   = false;
                s_client_recv_len = 0;
                if (event_handler) {
                    event_handler(NET_EVENT_CONNECTION_LOST, 0);
                }
            } else {
                int e = sock_errno();
                if (!WOULD_BLOCK(e)) {
                    MP_WARN(MP_CAT_NET, "recv from server error %d — connection lost", e);
                    closesock(client_sock);
                    client_sock = INVALID_SOCK;
                    is_active   = false;
                    s_client_recv_len = 0;
                    if (event_handler) {
                        event_handler(NET_EVENT_CONNECTION_LOST, 0);
                    }
                }
            }
        }
    }
}
