#ifndef ARCANUM_NET_NETWORK_H_
#define ARCANUM_NET_NETWORK_H_

#include <stdint.h>
#include <stdbool.h>

#define NET_PORT             12345
#define NET_MAX_MESSAGE_SIZE 16384
#define NET_MAX_CLIENTS      8

// Event types — passed to NetEventHandler along with the client slot id.
// client_id is 0 for NET_EVENT_CONNECTION_LOST (only one server connection
// from the client's perspective).
#define NET_EVENT_CLIENT_CONNECTED    1
#define NET_EVENT_CLIENT_DISCONNECTED 2
#define NET_EVENT_CONNECTION_LOST     3

typedef void (*NetMessageHandler)(void* msg);
typedef void (*NetEventHandler)(int event_type, int client_id);

// Global configuration — set these before calling net_start_* functions.
extern unsigned int g_server_options;   // Bitmask of TIG_NET_SERVER_* flags
extern char         g_mp_join_address[256]; // Target host for net_start_client
extern int          g_mp_max_players;   // Enforced on accept (default 8)

// Lifecycle
bool net_init(void);
void net_cleanup(void);

// Server
bool net_start_server(void);
void net_stop_server(void);

// Client
bool net_start_client(const char* host);
void net_stop_client(void);

// Status
bool net_is_active(void);
bool net_is_host(void);
bool net_client_is_connected(int client_id);
int  net_client_count(void);

// Server options
unsigned int net_get_server_options(void);
void         net_set_server_options(unsigned int opts);

// Messaging
void net_send_message(void* msg, int size);                           // broadcast
void net_send_message_to(int client_id, void* msg, int size);         // targeted
void net_send_message_except(int except_client_id, void* msg, int size); // all except

// Callbacks
void net_set_message_handler(NetMessageHandler handler);
void net_set_event_handler(NetEventHandler handler);

// Call once per frame from game loop
void net_poll(void);

#endif /* ARCANUM_NET_NETWORK_H_ */
