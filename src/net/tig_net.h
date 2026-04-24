#ifndef TIG_NET_H_
#define TIG_NET_H_

void tig_net_end(void);
void tig_net_pre_map_load(void);
void tig_net_post_map_load(void);
void tig_net_game_start(void);
void tig_net_session_ready(void);
void tig_net_player_ready(int player);
int tig_net_local_id(void);
int tig_net_session_list_count(void);
void tig_net_session_join(void);

#endif
