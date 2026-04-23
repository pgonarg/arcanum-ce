#ifndef ARCANUM_NET_COMPAT_H_
#define ARCANUM_NET_COMPAT_H_

#include "net/network.h"

// Network server options
#define TIG_NET_SERVER_PLAYER_KILLING 0x0001
#define TIG_NET_SERVER_FRIENDLY_FIRE 0x0002
#define TIG_NET_SERVER_AUTO_EQUIP 0x0020
#define TIG_NET_SERVER_KEY_SHARING 0x0040

// TIG_OK return code
#define TIG_OK 0

// Implemented network functions (redirected to src/net/network.c)
#define tig_net_is_active() net_is_active()
#define tig_net_is_host() net_is_host()
#define tig_net_send_app_all(msg, size) net_send_message(msg, size)
#define tig_net_start_client() (net_start_client("localhost") ? TIG_OK : 1)
#define tig_net_on_message(handler) net_set_message_handler(handler)
#define tig_net_on_network_event(handler) net_set_event_handler(handler)
#define tig_net_start_server() net_start_server()

// Stub implementations (not critical for Phase 3)
#define tig_net_send_app(a, b, c)
#define tig_net_send_app_except(a, b, c)
#define tig_net_local_client_set_name(a) 1
#define tig_net_local_server_set_max_players(a) 1
#define tig_net_local_server_set_description(a) 1
#define tig_net_local_server_set_name(a)
#define tig_net_on_message_validation(a)
#define sub_5280F0() 1
#define sub_52A940()
#define sub_52A950()
#define sub_52B210()
#define sub_5286E0()
#define tig_net_xfer_count(a) 0
#define tig_net_client_is_active(a) 0
#define tig_net_client_is_waiting(a) 0
#define tig_net_client_is_loading(a) 0
#define sub_52A9E0(a)
#define tig_net_local_server_get_max_players() 8
#define tig_net_xfer_send_as(a, b, c, d)
#define tig_net_xfer_send(a, b, c)
#define sub_529520() 0
#define tig_net_reset_connection()
#define tig_net_local_server_get_options() 0
#define sub_52A530() 0
#define sub_52A900() 0
#define tig_net_client_info_get_name(a) 0

#endif /* ARCANUM_NET_COMPAT_H_ */
