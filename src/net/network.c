#include "network.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define SOCKET_ERROR_VAL SOCKET_ERROR
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define closesocket_compat closesocket
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define SOCKET_ERROR_VAL -1
    #define INVALID_SOCKET_VAL -1
    #define closesocket_compat close
    typedef int SOCKET;
#endif

#include <string.h>
#include <stdio.h>
#include <time.h>

// Simple logging to file
static void net_log(const char* format, ...)
{
    FILE* log_file;
    va_list args;
    time_t now;
    struct tm* timeinfo;
    char timestamp[32];

    log_file = fopen("network.log", "a");
    if (log_file == NULL) {
        return;
    }

    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);

    fprintf(log_file, "[%s] ", timestamp);
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fprintf(log_file, "\n");
    fflush(log_file);
    fclose(log_file);
}

#include <stdarg.h>

// Global state
static SOCKET server_socket = INVALID_SOCKET_VAL;
static SOCKET client_socket = INVALID_SOCKET_VAL;
static SOCKET connected_client = INVALID_SOCKET_VAL;
static bool is_host = false;
static bool is_active = false;
static bool initialized = false;

// Handlers
static NetMessageHandler message_handler = NULL;
static NetEventHandler event_handler = NULL;

// Message queue
typedef struct {
    uint8_t buffer[NET_MAX_MESSAGE_SIZE];
    int length;
} QueuedMessage;

static QueuedMessage message_queue[32];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

// Helper to check if queue is full
static bool queue_is_full(void)
{
    return queue_count >= 32;
}

// Helper to enqueue a message
static bool queue_enqueue(const uint8_t* data, int length)
{
    if (queue_is_full() || length > NET_MAX_MESSAGE_SIZE) {
        return false;
    }

    memcpy(message_queue[queue_tail].buffer, data, length);
    message_queue[queue_tail].length = length;
    queue_tail = (queue_tail + 1) % 32;
    queue_count++;
    return true;
}

// Helper to dequeue a message
static bool queue_dequeue(uint8_t* data, int* length)
{
    if (queue_count == 0) {
        return false;
    }

    *length = message_queue[queue_head].length;
    memcpy(data, message_queue[queue_head].buffer, *length);
    queue_head = (queue_head + 1) % 32;
    queue_count--;
    return true;
}

// Set socket to non-blocking
static void set_nonblocking(SOCKET sock)
{
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Initialize Winsock (Windows only)
bool net_init(void)
{
    if (initialized) {
        return true;
    }

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return false;
    }
#endif

    initialized = true;
    return true;
}

// Cleanup
void net_cleanup(void)
{
    if (is_host) {
        net_stop_server();
    } else if (is_active) {
        net_stop_client();
    }

#ifdef _WIN32
    if (initialized) {
        WSACleanup();
    }
#endif

    initialized = false;
}

// Start server
bool net_start_server(void)
{
    struct sockaddr_in server_addr;

    if (!net_init()) {
        net_log("Failed to initialize network");
        return false;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET_VAL) {
        net_log("Failed to create server socket");
        return false;
    }

    set_nonblocking(server_socket);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(NET_PORT);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR_VAL) {
        net_log("Failed to bind server socket to port %d", NET_PORT);
        closesocket_compat(server_socket);
        server_socket = INVALID_SOCKET_VAL;
        return false;
    }

    if (listen(server_socket, 1) == SOCKET_ERROR_VAL) {
        net_log("Failed to listen on server socket");
        closesocket_compat(server_socket);
        server_socket = INVALID_SOCKET_VAL;
        return false;
    }

    net_log("SERVER: Listening on localhost:%d", NET_PORT);
    is_host = true;
    is_active = true;
    connected_client = INVALID_SOCKET_VAL;
    return true;
}

// Stop server
void net_stop_server(void)
{
    if (connected_client != INVALID_SOCKET_VAL) {
        closesocket_compat(connected_client);
        connected_client = INVALID_SOCKET_VAL;
    }

    if (server_socket != INVALID_SOCKET_VAL) {
        closesocket_compat(server_socket);
        server_socket = INVALID_SOCKET_VAL;
    }

    is_host = false;
    is_active = false;
}

// Start client
bool net_start_client(const char* host)
{
    struct sockaddr_in server_addr;
    const char* target_host = (host != NULL) ? host : "127.0.0.1";

    if (!net_init()) {
        net_log("Failed to initialize network");
        return false;
    }

    net_log("CLIENT: Attempting to connect to %s:%d", target_host, NET_PORT);

    client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_socket == INVALID_SOCKET_VAL) {
        net_log("Failed to create client socket");
        return false;
    }

    set_nonblocking(client_socket);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(target_host);
    server_addr.sin_port = htons(NET_PORT);

    // Non-blocking connect will return EINPROGRESS/WSAEWOULDBLOCK
    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR_VAL) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            net_log("Connect failed with error %d", err);
            closesocket_compat(client_socket);
            client_socket = INVALID_SOCKET_VAL;
            return false;
        }
#else
        int err = errno;
        if (err != EINPROGRESS && err != EWOULDBLOCK) {
            net_log("Connect failed with error %d", err);
            closesocket_compat(client_socket);
            client_socket = INVALID_SOCKET_VAL;
            return false;
        }
#endif
    }

    net_log("CLIENT: Connection initiated");
    is_host = false;
    is_active = true;
    return true;
}

// Stop client
void net_stop_client(void)
{
    if (client_socket != INVALID_SOCKET_VAL) {
        closesocket_compat(client_socket);
        client_socket = INVALID_SOCKET_VAL;
    }

    is_active = false;
}

// Check if network is active
bool net_is_active(void)
{
    return is_active;
}

// Check if we're hosting
bool net_is_host(void)
{
    return is_host;
}

// Send message
void net_send_message(void* msg, int size)
{
    int total_size;
    uint8_t buffer[NET_MAX_MESSAGE_SIZE];
    int sent;

    if (!is_active || size > NET_MAX_MESSAGE_SIZE - 4) {
        return;
    }

    // Frame: 4-byte length + data
    total_size = size + 4;
    *(int*)buffer = size;
    memcpy(buffer + 4, msg, size);

    if (is_host) {
        // Server broadcasts to connected client
        if (connected_client != INVALID_SOCKET_VAL) {
            sent = send(connected_client, (const char*)buffer, total_size, 0);
            if (sent == SOCKET_ERROR_VAL) {
                net_log("SERVER: Send failed, client disconnected");
                closesocket_compat(connected_client);
                connected_client = INVALID_SOCKET_VAL;
                if (event_handler != NULL) {
                    event_handler(NET_EVENT_CLIENT_DISCONNECTED);
                }
            } else {
                net_log("SERVER: Sent %d bytes", sent);
            }
        }
    } else {
        // Client sends to server
        if (client_socket != INVALID_SOCKET_VAL) {
            sent = send(client_socket, (const char*)buffer, total_size, 0);
            if (sent == SOCKET_ERROR_VAL) {
                net_log("CLIENT: Send failed, connection lost");
                closesocket_compat(client_socket);
                client_socket = INVALID_SOCKET_VAL;
                is_active = false;
                if (event_handler != NULL) {
                    event_handler(NET_EVENT_CONNECTION_LOST);
                }
            } else {
                net_log("CLIENT: Sent %d bytes", sent);
            }
        }
    }
}

// Set message handler
void net_set_message_handler(NetMessageHandler handler)
{
    message_handler = handler;
}

// Set event handler
void net_set_event_handler(NetEventHandler handler)
{
    event_handler = handler;
}

// Poll for incoming messages and events
void net_poll(void)
{
    fd_set read_set;
    struct timeval timeout;
    int activity;
    SOCKET listen_socket;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    uint8_t buffer[NET_MAX_MESSAGE_SIZE + 4];
    int received;
    int msg_length;
    uint8_t dequeued[NET_MAX_MESSAGE_SIZE];
    int deq_length;

    if (!is_active) {
        return;
    }

    // Process queued messages first
    while (queue_dequeue(dequeued, &deq_length)) {
        if (message_handler != NULL) {
            message_handler(dequeued);
        }
    }

    // Server: accept connections and receive from client
    if (is_host) {
        FD_ZERO(&read_set);
        FD_SET(server_socket, &read_set);
        if (connected_client != INVALID_SOCKET_VAL) {
            FD_SET(connected_client, &read_set);
        }

        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        activity = select((int)((connected_client > server_socket) ? connected_client : server_socket) + 1,
                          &read_set, NULL, NULL, &timeout);

        if (activity > 0) {
            // Check for new connections
            if (FD_ISSET(server_socket, &read_set)) {
                client_addr_len = sizeof(client_addr);
                listen_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
                if (listen_socket != INVALID_SOCKET_VAL) {
                    if (connected_client != INVALID_SOCKET_VAL) {
                        net_log("SERVER: Rejected second client connection");
                        closesocket_compat(listen_socket);
                    } else {
                        net_log("SERVER: Client connected from %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                        connected_client = listen_socket;
                        set_nonblocking(connected_client);
                        if (event_handler != NULL) {
                            event_handler(NET_EVENT_CLIENT_CONNECTED);
                        }
                    }
                }
            }

            // Receive from connected client
            if (connected_client != INVALID_SOCKET_VAL && FD_ISSET(connected_client, &read_set)) {
                received = recv(connected_client, (char*)buffer, sizeof(buffer), 0);
                if (received > 4) {
                    msg_length = *(int*)buffer;
                    if (msg_length > 0 && msg_length <= received - 4 && msg_length <= NET_MAX_MESSAGE_SIZE) {
                        queue_enqueue(buffer + 4, msg_length);
                        net_log("SERVER: Received message (%d bytes)", msg_length);
                    }
                } else if (received == 0 || received == SOCKET_ERROR_VAL) {
                    net_log("SERVER: Client disconnected");
                    closesocket_compat(connected_client);
                    connected_client = INVALID_SOCKET_VAL;
                    if (event_handler != NULL) {
                        event_handler(NET_EVENT_CLIENT_DISCONNECTED);
                    }
                }
            }
        }
    } else {
        // Client: receive from server
        FD_ZERO(&read_set);
        FD_SET(client_socket, &read_set);

        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        activity = select((int)client_socket + 1, &read_set, NULL, NULL, &timeout);

        if (activity > 0 && FD_ISSET(client_socket, &read_set)) {
            received = recv(client_socket, (char*)buffer, sizeof(buffer), 0);
            if (received > 4) {
                msg_length = *(int*)buffer;
                if (msg_length > 0 && msg_length <= received - 4 && msg_length <= NET_MAX_MESSAGE_SIZE) {
                    queue_enqueue(buffer + 4, msg_length);
                    net_log("CLIENT: Received message (%d bytes)", msg_length);
                }
            } else if (received == 0 || received == SOCKET_ERROR_VAL) {
                net_log("CLIENT: Connection lost");
                closesocket_compat(client_socket);
                client_socket = INVALID_SOCKET_VAL;
                is_active = false;
                if (event_handler != NULL) {
                    event_handler(NET_EVENT_CONNECTION_LOST);
                }
            }
        }
    }
}
