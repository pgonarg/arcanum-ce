#ifndef ARCANUM_NET_COMPAT_H_
#define ARCANUM_NET_COMPAT_H_

#include "net/network.h"

// ---------------------------------------------------------------------------
// Server option flags (checked throughout game code)
// ---------------------------------------------------------------------------
#define TIG_NET_SERVER_PLAYER_KILLING 0x0001
#define TIG_NET_SERVER_FRIENDLY_FIRE  0x0002
#define TIG_NET_SERVER_AUTO_EQUIP     0x0020
#define TIG_NET_SERVER_KEY_SHARING    0x0040

// Return code used by original TIG network API
#define TIG_OK 0

// ---------------------------------------------------------------------------
// Core network functions — mapped to real implementations
// ---------------------------------------------------------------------------
#define tig_net_is_active()              net_is_active()
#define tig_net_is_host()                net_is_host()
#define tig_net_send_app_all(msg, size)  net_send_message(msg, size)
#define tig_net_send_app(id, msg, size)  net_send_message_to(id, msg, size)
#define tig_net_send_app_except(id, msg, size) net_send_message_except(id, msg, size)
#define tig_net_start_server()           net_start_server()
#define tig_net_start_client()           (net_start_client(g_mp_join_address) ? TIG_OK : 1)
#define tig_net_on_message(h)            net_set_message_handler(h)
#define tig_net_on_network_event(h)      net_set_event_handler(h)

// Client slot status — backed by real connection tracking
#define tig_net_client_is_active(id)     net_client_is_connected(id)
#define tig_net_client_is_waiting(id)    0  // Not yet implemented
#define tig_net_client_is_loading(id)    0  // Not yet implemented

// Server options — backed by g_server_options global
#define tig_net_local_server_get_options()      net_get_server_options()
#define tig_net_local_server_set_options(opts)  net_set_server_options(opts)
#define tig_net_local_server_get_max_players()  g_mp_max_players
#define tig_net_local_server_set_max_players(n) (g_mp_max_players = (n), 1)
#define tig_net_local_server_set_name(n)        ((void)(n))
#define tig_net_local_server_set_description(n) ((void)(n))
#define tig_net_local_client_set_name(n)        1

// Message validation hook — not implemented, silently ignored
#define tig_net_on_message_validation(h)        ((void)(h))

// ---------------------------------------------------------------------------
// File transfer — not yet implemented (Phase 6).
// xfer_count returns 0 so the "wait for transfer" logic in multiplayer.c
// proceeds immediately. Files are not actually transferred until Phase 6.
// See MULTIPLAYER_IMPLEMENTATION_PLAN.md §Phase 6.
// ---------------------------------------------------------------------------
#define tig_net_xfer_count(id)              0
#define tig_net_xfer_send(path, id, cb)     ((void)0)
#define tig_net_xfer_send_as(s, d, id, cb) ((void)0)

// ---------------------------------------------------------------------------
// Stubs for original TIG functions that have no CE equivalent.
// These controlled lobby/session browsing in the original DirectPlay layer.
// ---------------------------------------------------------------------------
#define sub_5280F0()          1
#define sub_52A940()          ((void)0)
#define sub_52A950()          ((void)0)
#define sub_52B210()          ((void)0)
#define sub_5286E0()          ((void)0)
#define sub_52A9E0(a)         ((void)(a))
#define sub_529520()          0
#define tig_net_reset_connection()          ((void)0)
#define sub_52A530()          0
#define sub_52A900()          0
#define tig_net_client_info_get_name(id)    0

#endif /* ARCANUM_NET_COMPAT_H_ */
