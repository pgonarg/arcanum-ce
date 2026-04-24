#include "net/tig_net.h"

void tig_net_end(void) {}
void tig_net_pre_map_load(void) {}
void tig_net_post_map_load(void) {}
void tig_net_game_start(void) {}
void tig_net_session_ready(void) {}
void tig_net_player_ready(int player) {}
int tig_net_local_id(void) { return 0; }
int tig_net_session_list_count(void) { return 0; }
void tig_net_session_join(void) {}
