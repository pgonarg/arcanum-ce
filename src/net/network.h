#ifndef ARCANUM_NET_NETWORK_H_
#define ARCANUM_NET_NETWORK_H_

#include <stdint.h>
#include <stdbool.h>

#define NET_PORT 12345
#define NET_MAX_MESSAGE_SIZE 16384
#define NET_MAX_CLIENTS 8

typedef void (*NetMessageHandler)(void* msg);
typedef void (*NetEventHandler)(int event_type);

// Event types
#define NET_EVENT_CLIENT_CONNECTED 1
#define NET_EVENT_CLIENT_DISCONNECTED 2
#define NET_EVENT_CONNECTION_LOST 3

// Initialize/cleanup
bool net_init(void);
void net_cleanup(void);

// Server functions
bool net_start_server(void);
void net_stop_server(void);

// Client functions
bool net_start_client(const char* host);
void net_stop_client(void);

// Status
bool net_is_active(void);
bool net_is_host(void);

// Messaging
void net_send_message(void* msg, int size);
void net_set_message_handler(NetMessageHandler handler);
void net_set_event_handler(NetEventHandler handler);

// Poll (call from game loop)
void net_poll(void);

#endif /* ARCANUM_NET_NETWORK_H_ */
