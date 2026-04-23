# Arcanum CE — Multiplayer Implementation Plan

> **Scope**: Complete plan to get 2-player multiplayer working end-to-end.
> **Prerequisite reading**: `docs/MULTIPLAYER_ARCHITECTURE.md`
> **Decisions are made here — no "TBD" items.**

---

## Table of Contents

1. [What Gets Scrapped and Why](#1-what-gets-scrapped-and-why)
2. [Authority Model — Who Owns What](#2-authority-model--who-owns-what)
3. [Debug Logging System](#3-debug-logging-system)
4. [Phase 0 — Logging Infrastructure](#phase-0--logging-infrastructure)
5. [Phase 1 — Fix the Network Layer](#phase-1--fix-the-network-layer)
6. [Phase 2 — Fix net_compat.h Stubs](#phase-2--fix-net_compatch-stubs)
7. [Phase 3 — Fix the UI Flow](#phase-3--fix-the-ui-flow)
8. [Phase 4 — Implement Packet Handlers](#phase-4--implement-packet-handlers)
9. [Phase 5 — Bi-Directional Sync](#phase-5--bi-directional-sync)
10. [Phase 6 — Character File Transfer](#phase-6--character-file-transfer)
11. [Phase 7 — Clock Synchronization](#phase-7--clock-synchronization)
12. [Phase 8 — Server Options & Configuration](#phase-8--server-options--configuration)
13. [Phase 9 — Disconnection & Error Handling](#phase-9--disconnection--error-handling)
14. [Testing Checkpoints](#testing-checkpoints)
15. [File Change Index](#file-change-index)

---

## 1. What Gets Scrapped and Why

These are not "improvements" — they are decisions to throw away broken or fundamentally wrong code and replace it with something that can actually work.

---

### 1.1 The Single-Client Socket Model in `network.c`

**Current code**: `network.c` has a single `connected_client` socket. One client, ever.

**Problem**: The game supports up to 8 players. The server needs a socket array and must broadcast to all of them.

**Decision**: Replace `connected_client` with `client_sockets[NET_MAX_CLIENTS]` and a corresponding `client_count`. Broadcast in `net_send_message()` by iterating all connected sockets. Add `net_send_message_to(client_id, msg, size)` for targeted sends (needed for file transfer and join handshake).

---

### 1.2 The Per-Call `fopen`/`fclose` Logging in `network.c`

**Current code**: `net_log()` opens `network.log`, writes, and closes it on every single call.

**Problem**: This is a disk hit per log line. At 60 fps with network activity, this will tank performance. It also puts `#include <stdarg.h>` after the function that uses it — that's a compile error on strict compilers.

**Decision**: Throw away `net_log()`. Replace with the centralized logging system described in Phase 0. All files use the same log API.

---

### 1.3 The 8x Time Catchup Hack in `timeevent.c`

**Current code** (`timeevent.c:825-841`): When a client's `game_time` is behind the host, time runs at 8× speed until it catches up.

**Problem**: Animations, spell durations, buff timers, and all time-based events run 8× faster during catchup. The game becomes unwatchable. This was a lazy original-game hack that only worked because dial-up lag was the norm and nobody ever joined mid-session.

**Decision**: Remove the 8× multiplier entirely. Replace with NTP-style clock offset (see Phase 7). The client maintains a `host_time_offset` and adds it when comparing against host time. No catchup acceleration needed.

---

### 1.4 Non-Host Broadcasting Packet9 with Zero Data

**Current code** (`anim.c:4000-4025`): Non-host clients send `Packet9` to all players with position and art_id fields set to zero.

**Problem**: This is noise. The host receives a packet claiming a player has moved to position (0,0) with no art. If a Packet9 handler is ever added, this will corrupt the host's state. The intent was for the host to re-broadcast, but there's no such relay logic.

**Decision**: Remove the non-host broadcast of Packet9. Non-host clients never broadcast animation state — they send movement intent to the host (Packet27 request), and the host broadcasts the resulting state. This is cleaner and matches the authority model.

The specific code block at `anim.c:4000-4025` should be rewritten as:
```c
if (tig_net_is_active() && tig_net_is_host()) {
    // Only host broadcasts animation state
    Packet9 pkt;
    // ... fill with real data ...
    tig_net_send_app_all(&pkt, sizeof(pkt));
}
// Non-host: nothing. Movement was already sent as intent in intgame.c.
```

---

### 1.5 The AnimID Slot-Number Equality Hack

**Current code** (`anim.c:2806-2829`): In multiplayer, `anim_id_run_is_equal()` ignores `slot_num` because local slot numbers don't match across the network.

**Problem**: This weakens animation identity globally during a multiplayer session. Two different animations with the same `unique_id` can incorrectly cancel each other.

**Decision**: The real fix is to never send raw `AnimID` structs over the network. Instead, packets carry semantic animation state (animation type enum + object ID + parameters), not local slot references. The `AnimID` equality hack can then be removed — slot_num comparison can go back to always being part of equality.

This is a consequence of Phase 4 (implementing animation packet handlers properly) and Phase 5 (sending semantic animation intent rather than raw IDs).

---

### 1.6 The 32-Entry Message Queue

**Current code** (`network.c:74`): Fixed array of 32 messages, each up to 16 KB.

**Problem**: Memory use is 32 × 16 KB = 512 KB always allocated. More importantly, 32 entries is too small for burst scenarios (fast spell sequences, entering a crowded area). When full, messages are silently dropped.

**Decision**: Replace with a single flat ring buffer per client: a byte array of 256 KB with head/tail byte offsets. Messages are framed with their length inline. This handles burst without silent drops and uses memory proportional to actual load.

---

### 1.7 Server Binding to 127.0.0.1

**Current code** (`network.c:181`): `server_addr.sin_addr.s_addr = inet_addr("127.0.0.1")` — server only accepts local connections.

**Decision**: Bind to `INADDR_ANY` (0.0.0.0). This accepts both LAN and localhost connections. For security, firewall is the user's responsibility.

---

## 2. Authority Model — Who Owns What

This defines exactly which machine executes what. These decisions drive all of Phase 4 and 5.

### Host-Authoritative (Host executes, then broadcasts result)

The host is the single source of truth for:

| Domain | Why |
|--------|-----|
| All item state (pickup, drop, equip, unequip) | Prevents duplicate pickup / inventory desync |
| Combat damage calculation | Prevents cheating, ensures determinism |
| Spell effects and summons | Complex side-effects must be consistent |
| NPC / AI / follower behavior | AI is single-threaded on the host |
| Object creation and destruction | Object IDs must be globally unique |
| Game time | One clock, all clients follow it |
| Script execution (indices 1 and 3) | Script side-effects must run once |
| Party membership | Authoritative party table |
| Map transitions | World state |

### Client-Predicted (Client acts locally, sends intent to host, host corrects if wrong)

| Domain | Why |
|--------|-----|
| Local player movement | Feels responsive; host corrects if out of bounds or blocked |
| Local player facing/animation start | Visual responsiveness; corrected by next position sync |

### Client-Local (No sync, no packets)

| Domain | Why |
|--------|-----|
| Camera | Entirely local |
| UI state (windows open/closed) | Local player experience |
| Sound effects | Triggered by received events, but playback is local |
| Cursor/hover effects | Input-driven, local |
| Fade/transition effects | Scripted, local (MP suppresses these anyway) |

### The Consequences of These Decisions

- Clients send **intent packets** for their local player actions (move to X, attack Y, use item Z).
- The host validates and **executes the action**, then broadcasts the **result** to all clients including the originator.
- Clients apply the result. If the result differs from what the client predicted (e.g., movement blocked), the client snaps to the corrected state.
- Clients **never directly execute** item operations, spell effects, combat damage, or AI decisions.

This matches the existing code structure — the non-host early-returns in `magictech.c`, `item.c`, `anim.c` are correct. The missing piece is the handlers that apply the host's broadcast results on clients.

---

## 3. Debug Logging System

This is designed upfront because every phase uses it. No phase ships without log coverage.

### 3.1 Design Requirements

- **File only** — no console, no stdout, no OutputDebugString
- **Always-on in debug builds, opt-in in release**
- **Levels**: ERROR, WARN, INFO, DEBUG, TRACE (increasing verbosity)
- **Categories**: NET, PKT, SYNC, ANIM, SPELL, ITEM, PARTY, TIME, SESSION, UI
- **Performance**: File kept open, flushed periodically (not per-line except on ERROR)
- **Format**: `[HH:MM:SS.mmm] [LEVEL] [CATEGORY] message`
- **Crash safety**: Ring buffer flushed on ERROR; log closed on shutdown
- **No allocation**: Log calls must be safe from any context including signal handlers

### 3.2 Log File

Single file: `arcanum_mp.log` in the game working directory. Overwritten on each session start (not appended — prevents multi-GB log files from long play sessions). Previous session's log overwritten. If the user needs to retain a session, they copy the file.

### 3.3 API (new file: `src/net/mp_log.h`)

```c
// Log levels
#define MP_LOG_ERROR  0
#define MP_LOG_WARN   1
#define MP_LOG_INFO   2
#define MP_LOG_DEBUG  3
#define MP_LOG_TRACE  4

// Categories
#define MP_CAT_NET      "NET"
#define MP_CAT_PKT      "PKT"
#define MP_CAT_SYNC     "SYNC"
#define MP_CAT_ANIM     "ANIM"
#define MP_CAT_SPELL    "SPELL"
#define MP_CAT_ITEM     "ITEM"
#define MP_CAT_PARTY    "PARTY"
#define MP_CAT_TIME     "TIME"
#define MP_CAT_SESSION  "SESSION"
#define MP_CAT_UI       "UI"

// Initialize/shutdown
void mp_log_init(void);    // Call once at startup; opens file, writes header
void mp_log_shutdown(void); // Flush and close

// Core logging function (do not call directly)
void mp_log_write(int level, const char* category, const char* fmt, ...);

// Convenience macros
#define MP_ERROR(cat, ...)  mp_log_write(MP_LOG_ERROR, cat, __VA_ARGS__)
#define MP_WARN(cat, ...)   mp_log_write(MP_LOG_WARN,  cat, __VA_ARGS__)
#define MP_INFO(cat, ...)   mp_log_write(MP_LOG_INFO,  cat, __VA_ARGS__)
#define MP_DEBUG(cat, ...)  mp_log_write(MP_LOG_DEBUG, cat, __VA_ARGS__)
#define MP_TRACE(cat, ...)  mp_log_write(MP_LOG_TRACE, cat, __VA_ARGS__)

// Packet-specific helper: logs packet type + size + direction
#define MP_LOG_SEND(pkt_type, size) \
    MP_TRACE(MP_CAT_PKT, "SEND type=%d size=%d", (pkt_type), (size))
#define MP_LOG_RECV(pkt_type, size) \
    MP_TRACE(MP_CAT_PKT, "RECV type=%d size=%d", (pkt_type), (size))
```

### 3.4 Implementation Notes

- Keep the file handle open as a `static FILE*` inside `mp_log.c`.
- `mp_log_write()` formats into a stack buffer (`char buf[1024]`), writes with a single `fwrite()`.
- Flush on every ERROR. Flush INFO and above every 1 second (tracked by checking `clock()` on each call).
- TRACE and DEBUG only flushed on shutdown or when triggered by ERROR.
- Compile-time gate: `#if defined(MP_LOG_LEVEL)` controls which calls compile to nothing in release. Define `MP_LOG_LEVEL 4` in debug builds, `MP_LOG_LEVEL 2` in release.

### 3.5 Log Entries That Must Exist (Non-Negotiable)

Every phase must add these log entries at minimum:

| Event | Level | Category |
|-------|-------|----------|
| Session start (host or client) | INFO | SESSION |
| Player connected (host sees client join) | INFO | SESSION |
| Player disconnected | WARN | SESSION |
| Connection lost (client) | WARN | NET |
| Every packet sent (type + size) | TRACE | PKT |
| Every packet received (type + size) | TRACE | PKT |
| Unknown packet type received | WARN | PKT |
| Handler called but game object not found | WARN | SYNC |
| File transfer start/end | INFO | SESSION |
| Clock sync offset computed | DEBUG | TIME |
| Map transition triggered by packet | INFO | SYNC |
| `tig_net_client_is_active` returning true for slot N | DEBUG | SESSION |

---

## Phase 0 — Logging Infrastructure

**Goal**: Get the log system in place before anything else. All subsequent phases log to it.

**Files to create**:
- `src/net/mp_log.h` — API as described in §3.3
- `src/net/mp_log.c` — Implementation

**Files to modify**:
- `src/net/network.c` — Remove `net_log()`, replace all calls with `MP_*(MP_CAT_NET, ...)`. Fix the `#include <stdarg.h>` placement. Fix include guards.
- `CMakeLists.txt` (or equivalent build file) — Add `mp_log.c` to the build.

**Implementation of `mp_log.c`**:

```c
#include "mp_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static FILE* log_file = NULL;
static const char* level_names[] = { "ERROR", "WARN ", "INFO ", "DEBUG", "TRACE" };

void mp_log_init(void)
{
    log_file = fopen("arcanum_mp.log", "w");
    if (!log_file) return;
    fprintf(log_file, "=== Arcanum CE Multiplayer Log ===\n");
    fflush(log_file);
}

void mp_log_shutdown(void)
{
    if (!log_file) return;
    fprintf(log_file, "=== Session End ===\n");
    fclose(log_file);
    log_file = NULL;
}

void mp_log_write(int level, const char* category, const char* fmt, ...)
{
    char msg[1024];
    va_list args;
    time_t now;
    struct tm* t;
    char timestamp[32];

    if (!log_file) return;
    if (level > MP_LOG_LEVEL) return;

    time(&now);
    t = localtime(&now);
    // HH:MM:SS — milliseconds require platform-specific calls, use seconds for now
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", t);

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    fprintf(log_file, "[%s] [%s] [%s] %s\n",
            timestamp, level_names[level], category, msg);

    if (level <= MP_LOG_ERROR) {
        fflush(log_file);
    }
}
```

**Call `mp_log_init()` from `multiplayer_init()`** and `mp_log_shutdown()` from `multiplayer_exit()`. This ties log lifetime to the multiplayer module lifecycle.

**Deliverable**: Build compiles with the new log system. Running the game creates `arcanum_mp.log` with a session header even before any network activity.

---

## Phase 1 — Fix the Network Layer

**Goal**: `network.c` supports up to 8 simultaneous client connections, properly handles TCP stream framing, and uses the logging system.

**Files to modify**: `src/net/network.c`, `src/net/network.h`

### 1.1 Multi-Client Support

Replace the single `connected_client` socket with an array:

```c
#define CLIENT_SLOT_INVALID  -1

typedef struct {
    SOCKET  sock;
    bool    connected;
    uint8_t recv_buf[NET_MAX_MESSAGE_SIZE * 4]; // stream reassembly buffer
    int     recv_buf_len;                        // bytes currently in buffer
} ClientSlot;

static ClientSlot clients[NET_MAX_CLIENTS];
static int client_count = 0;
```

On `net_start_server()`, zero-initialize all slots. On accept, find the first slot where `connected == false`.

### 1.2 TCP Stream Framing

**Critical bug in current code**: A single `recv()` call may return a partial message or multiple messages concatenated. The current code assumes `recv()` returns exactly one complete message — this is wrong for TCP.

The fix is a per-client receive buffer with a length-prefix framing protocol:

```
[ 4 bytes: message length (uint32_t, little-endian) ] [ N bytes: message data ]
```

In `net_poll()` for each connected client:

```c
// Append new bytes to reassembly buffer
int received = recv(slot->sock, slot->recv_buf + slot->recv_buf_len,
                    sizeof(slot->recv_buf) - slot->recv_buf_len, 0);
if (received > 0) {
    slot->recv_buf_len += received;
}

// Extract complete messages from buffer
while (slot->recv_buf_len >= 4) {
    uint32_t msg_len = *(uint32_t*)slot->recv_buf;
    if (msg_len == 0 || msg_len > NET_MAX_MESSAGE_SIZE) {
        MP_ERROR(MP_CAT_NET, "Framing error: bad length %u from client %d", msg_len, i);
        disconnect_client(i);
        break;
    }
    if (slot->recv_buf_len < 4 + (int)msg_len) {
        break; // Wait for more data
    }
    // Dispatch complete message
    if (message_handler) {
        MP_LOG_RECV(*(int*)(slot->recv_buf + 4), msg_len);
        message_handler(slot->recv_buf + 4);
    }
    // Shift buffer
    int total = 4 + (int)msg_len;
    memmove(slot->recv_buf, slot->recv_buf + total, slot->recv_buf_len - total);
    slot->recv_buf_len -= total;
}
```

### 1.3 Sending

Add `net_send_message_to(int client_id, void* msg, int size)` for targeted sends. Modify `net_send_message()` to broadcast to all connected clients. Add framing (length prefix) on send:

```c
void net_send_message(void* msg, int size)
{
    // Broadcast: send to all connected clients (if host)
    // or send to server socket (if client)
    uint8_t framed[NET_MAX_MESSAGE_SIZE + 4];
    *(uint32_t*)framed = (uint32_t)size;
    memcpy(framed + 4, msg, size);
    MP_LOG_SEND(*(int*)msg, size);

    if (is_host) {
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (clients[i].connected)
                send_to_slot(i, framed, size + 4);
        }
    } else {
        send_all(client_socket, framed, size + 4);
    }
}
```

Add `send_to_slot()` that handles partial sends (loops until all bytes sent or error):

```c
static void send_to_slot(int slot_id, const uint8_t* data, int size)
{
    int sent = 0;
    while (sent < size) {
        int result = send(clients[slot_id].sock, (const char*)data + sent, size - sent, 0);
        if (result <= 0) {
            MP_WARN(MP_CAT_NET, "Send failed to client %d, disconnecting", slot_id);
            disconnect_client(slot_id);
            return;
        }
        sent += result;
    }
}
```

### 1.4 Client Tracking Functions

Add to `network.h`:

```c
bool net_client_is_connected(int client_id);  // Returns true if slot is active
int  net_client_count(void);                  // Returns number of connected clients
void net_send_message_to(int client_id, void* msg, int size);
```

These replace the `tig_net_client_is_active(a)` stub. Update `net_compat.h` accordingly.

### 1.5 Event Handler Signature

The current `NetEventHandler` signature `(int event_type)` loses the client ID. Change to:

```c
typedef void (*NetEventHandler)(int event_type, int client_id);
```

Client ID is always 0 for the "connection lost" event on the client side (there's only one connection), but on the host side it identifies which client connected/disconnected.

### 1.6 Server Bind Address

Change `inet_addr("127.0.0.1")` to `INADDR_ANY` so the server accepts connections from the LAN, not just localhost.

### 1.7 `select()` fd_set for Multiple Clients

The host's `select()` call must include all connected client sockets plus the listen socket. Build the fd_set dynamically:

```c
FD_ZERO(&read_set);
FD_SET(server_socket, &read_set);
SOCKET max_fd = server_socket;
for (int i = 0; i < NET_MAX_CLIENTS; i++) {
    if (clients[i].connected) {
        FD_SET(clients[i].sock, &read_set);
        if (clients[i].sock > max_fd) max_fd = clients[i].sock;
    }
}
select((int)max_fd + 1, &read_set, NULL, NULL, &timeout);
```

**Deliverable**: Server accepts 2–8 simultaneous clients. TCP stream is correctly reassembled. All network events logged. Build clean.

---

## Phase 2 — Fix net_compat.h Stubs

**Goal**: Every stub that's required for the current game code to function correctly is replaced with a real call. Stubs that are truly optional remain as no-ops but are documented as such.

**File to modify**: `src/net_compat.h`

### 2.1 Required Fixes

| Stub | Fix |
|------|-----|
| `tig_net_client_is_active(a)` returns 0 | → `net_client_is_connected(a)` |
| `tig_net_send_app(client, msg, size)` no-op | → `net_send_message_to(client, msg, size)` |
| `tig_net_send_app_except(except, msg, size)` no-op | → new `net_send_message_except(except, msg, size)` function |
| `tig_net_start_client()` hardcoded IP | → `net_start_client(g_mp_join_address)` where `g_mp_join_address` is set by UI |
| `tig_net_local_server_get_options()` returns 0 | → `net_get_server_options()` backed by `g_server_options` global |
| `tig_net_local_server_set_max_players(n)` no-op | → `net_set_max_players(n)` |

### 2.2 File Transfer Stubs

`tig_net_xfer_*` functions are needed for character file transfer. For now they remain stubs, and the character file transfer mechanism is redesigned in Phase 6. When Phase 6 is implemented, these stubs are replaced. Until then, document them clearly:

```c
// FILE TRANSFER: Not yet implemented. See Phase 6 / MULTIPLAYER_IMPLEMENTATION_PLAN.md.
// These are called by multiplayer.c for character file sync. Current behavior:
// xfer_count always 0 (sync "completes" immediately, files not actually sent).
// Phase 6 will replace these with in-band binary transfer.
#define tig_net_xfer_count(a)          0
#define tig_net_xfer_send(a, b, c)
#define tig_net_xfer_send_as(a, b, c, d)
```

### 2.3 Server Options Global

Add to `src/net/network.h`:

```c
extern unsigned int g_server_options;  // Bitfield of TIG_NET_SERVER_* flags
extern char g_mp_join_address[256];    // Address entered by client in UI
extern int  g_mp_max_players;

unsigned int net_get_server_options(void);
void net_set_server_options(unsigned int opts);
```

Defined in `network.c`:
```c
unsigned int g_server_options  = 0;
char g_mp_join_address[256]    = "127.0.0.1";
int  g_mp_max_players          = 2;
```

**Deliverable**: `tig_net_client_is_active()` returns real data. Party iteration finds connected players. Build clean, single-player still works.

---

## Phase 3 — Fix the UI Flow

**Goal**: Clicking Host starts the server. Clicking Join starts the client with the correct address. The game actually enters multiplayer mode.

**Files to modify**: `src/ui/mainmenu_ui.c`, `src/net/network.h` (for global state)

### 3.1 Game Mode Global

Add to `src/net/network.h` (or a new `src/game/mp_state.h`):

```c
typedef enum {
    GAME_MODE_SINGLE_PLAYER  = 0,
    GAME_MODE_MP_HOST        = 1,
    GAME_MODE_MP_CLIENT      = 2,
} GameMode;

extern GameMode g_game_mode;
```

Defined in `network.c` (or `mp_state.c`):
```c
GameMode g_game_mode = GAME_MODE_SINGLE_PLAYER;
```

### 3.2 Button Handler Fix

In `mainmenu_ui_multiplayer_button_released()` (currently at `mainmenu_ui.c:~916`):

```c
static void mainmenu_ui_multiplayer_button_released(int button_idx)
{
    switch (button_idx) {
    case 0: // Join Game
        g_game_mode = GAME_MODE_MP_CLIENT;
        MP_INFO(MP_CAT_UI, "User selected: Join Game");
        mainmenu_ui_open_window(MM_WINDOW_PICK_NEW_OR_PREGEN);
        // TODO Phase 3.3: open address input before pick window
        break;
    case 1: // Host Game
        g_game_mode = GAME_MODE_MP_HOST;
        MP_INFO(MP_CAT_UI, "User selected: Host Game");
        mainmenu_ui_open_window(MM_WINDOW_PICK_NEW_OR_PREGEN);
        break;
    case 2: // Back
        mainmenu_ui_open_window(MM_WINDOW_MAINMENU);
        break;
    }
}
```

### 3.3 Address Input

The join address input window already exists (`mainmenu_ui.c:943-967`). Fix it to write the entered text into `g_mp_join_address` (declared in `network.h`) before proceeding to character selection. The window's "Connect" button should:

```c
// In the Connect button handler of the address input window:
strncpy(g_mp_join_address, address_input_field_text, sizeof(g_mp_join_address) - 1);
g_mp_join_address[sizeof(g_mp_join_address) - 1] = '\0';
MP_INFO(MP_CAT_UI, "Join address set to: %s", g_mp_join_address);
mainmenu_ui_open_window(MM_WINDOW_PICK_NEW_OR_PREGEN);
```

For the Host path, show the address input window too but pre-fill it with the server IP (for the user to share with others) and make the field read-only. This is polish — for MVP, just skip the host address display.

Insert the address window into the join flow: `MM_WINDOW_MULTIPLAYER (join button) → address input window → MM_WINDOW_PICK_NEW_OR_PREGEN`.

### 3.4 Game Start Branch in `sub_5412E0()`

This is the most critical fix. In `sub_5412E0()` at `src/ui/mainmenu_ui.c:1664`, add before the existing single-player startup:

```c
static void sub_5412E0(bool a1)
{
    if (!mainmenu_ui_active) {
        intgame_refresh_cursor();
        intgame_show();
        return;
    }

    gameuilib_wants_mainmenu_unset();

    // --- MULTIPLAYER STARTUP ---
    if (g_game_mode == GAME_MODE_MP_HOST) {
        MP_INFO(MP_CAT_SESSION, "Starting as HOST on port %d", NET_PORT);
        if (!sub_49CC50()) {  // calls tig_net_start_server()
            MP_ERROR(MP_CAT_SESSION, "Failed to start server");
            // TODO Phase 9: show error dialog
            mainmenu_ui_open_window(MM_WINDOW_MAINMENU);
            return;
        }
        tig_net_on_message(multiplayer_handle_message);
        tig_net_on_network_event(multiplayer_handle_network_event);
        MP_INFO(MP_CAT_SESSION, "Server started, waiting for client...");
        // Host continues to load game normally;
        // multiplayer_handle_network_event fires when client connects
    } else if (g_game_mode == GAME_MODE_MP_CLIENT) {
        MP_INFO(MP_CAT_SESSION, "Starting as CLIENT, connecting to %s", g_mp_join_address);
        if (!multiplayer_start()) {  // calls tig_net_start_client(g_mp_join_address)
            MP_ERROR(MP_CAT_SESSION, "Failed to connect to %s", g_mp_join_address);
            // TODO Phase 9: show error dialog
            mainmenu_ui_open_window(MM_WINDOW_MAINMENU);
            return;
        }
    }
    // --- END MULTIPLAYER STARTUP ---

    // Existing single-player startup (runs for all modes):
    pc_obj = player_get_local_pc_obj();
    if (mainmenu_ui_start_new_game) {
        // ... map load, teleport, etc. (unchanged) ...
    }

    mainmenu_ui_close(false);
    intgame_refresh_cursor();
    intgame_show();
}
```

**Note**: `tig_net_start_client()` must be changed to use `g_mp_join_address` (Phase 2 fix) for this to work.

### 3.5 Reset Game Mode on Return to Main Menu

In the back/quit logic, reset `g_game_mode = GAME_MODE_SINGLE_PLAYER` so a subsequent single-player game isn't treated as multiplayer.

**Deliverable**: Host game starts a listening server. Join game connects to the specified address. Both show up in `arcanum_mp.log`. Single-player unchanged.

---

## Phase 4 — Implement Packet Handlers

**Goal**: All packets sent by the game are actually processed when received. This is the largest phase.

**File to modify**: `src/game/multiplayer.c` — specifically `multiplayer_handle_message()` at line 978.

### 4.1 Handler Implementation Order

Implement in this order (most impactful first):

#### Priority 1 — Session Establishment

**Packet 0 — `PacketGamePlayerList`**

Sent by host to all clients when a client joins. Contains the ObjectIDs of all active players.

```c
case 0: {
    PacketGamePlayerList* pkt = (PacketGamePlayerList*)msg;
    MP_INFO(MP_CAT_SESSION, "Received player list from host");
    for (int i = 0; i < NUM_PLAYERS; i++) {
        if (pkt->oid[i].type != OID_TYPE_NULL) {
            stru_5E8AD0[i].field_8 = pkt->oid[i];
            MP_DEBUG(MP_CAT_SESSION, "  Slot %d: OID set", i);
        }
    }
    break;
}
```

**Packet 64 — `Packet64` (Map Load)**

Sent by host to tell a client which map to load.

```c
case 64: {
    Packet64* pkt = (Packet64*)msg;
    MP_INFO(MP_CAT_SYNC, "Received map load: map=%d name=%s", pkt->map, pkt->name);
    // Trigger map transition on client side
    map_open_by_name(pkt->name);
    // The host player (pkt->player) is also arriving at this map
    break;
}
```

**Packet 46 — Disconnect**

```c
case 46: {
    Packet46* pkt = (Packet46*)msg;
    MP_WARN(MP_CAT_SESSION, "Player disconnect packet received");
    // Remove player from world
    // (see Phase 9 for full disconnection handling)
    break;
}
```

#### Priority 2 — Position & Animation

**Packet 27 — Location Update** (already implemented, add logging)

```c
case 27: {
    Packet27* pkt = (Packet27*)msg;
    if (pkt->oid.type == OID_TYPE_NULL) break;
    int64_t obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        MP_WARN(MP_CAT_SYNC, "Packet27: OID not found locally");
        break;
    }
    MP_TRACE(MP_CAT_SYNC, "Packet27: moving object to new location");
    sub_4A1F30(obj, pkt->loc, 0, 0);
    break;
}
```

**Packet 1 — Game Time**

```c
case 1: {
    PacketGameTime* pkt = (PacketGameTime*)msg;
    // Apply clock offset (Phase 7 adds proper NTP sync)
    // For now: snap to host time if we're far behind
    MP_DEBUG(MP_CAT_TIME, "Received game time: %d (local: %d)",
             pkt->game_time, current_game_time);
    if (current_game_time < pkt->game_time) {
        current_game_time = pkt->game_time;
        MP_DEBUG(MP_CAT_TIME, "Snapped game time to host");
    }
    break;
}
```

**Packet 5 — Animation Goal**

```c
case 5: {
    Packet5* pkt = (Packet5*)msg;
    int64_t obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        MP_WARN(MP_CAT_ANIM, "Packet5: OID not found for animation goal");
        break;
    }
    // Apply animation goal to object (without re-broadcasting)
    anim_goal_apply(obj, pkt->goal_type, &pkt->location, pkt->anim_id);
    MP_TRACE(MP_CAT_ANIM, "Applied animation goal type=%d to object", pkt->goal_type);
    break;
}
```

#### Priority 3 — Party & Combat

**Packet 71 — Party Update**

```c
case 71: {
    PacketPartyUpdate* pkt = (PacketPartyUpdate*)msg;
    MP_DEBUG(MP_CAT_PARTY, "Received party table update from host");
    memcpy(dword_5FC32C, pkt->party, sizeof(pkt->party));
    break;
}
```

**Packet 26 — Combat Mode**

```c
case 26: {
    PacketCombatModeSet* pkt = (PacketCombatModeSet*)msg;
    int64_t obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        MP_WARN(MP_CAT_SYNC, "Packet26: OID not found for combat mode");
        break;
    }
    MP_DEBUG(MP_CAT_SYNC, "Combat mode %s for object",
             pkt->active ? "ENTER" : "EXIT");
    combat_mode_set(obj, pkt->active);
    break;
}
```

**Packet 72 — Object Destroy**

```c
case 72: {
    PacketObjectDestroy* pkt = (PacketObjectDestroy*)msg;
    int64_t obj = obj_pool_perm_lookup(pkt->oid);
    if (obj == OBJ_HANDLE_NULL) {
        MP_WARN(MP_CAT_SYNC, "Packet72: OID not found for destroy");
        break;
    }
    MP_INFO(MP_CAT_SYNC, "Destroying object by host request");
    object_destroy(obj);
    break;
}
```

#### Priority 4 — Items & Inventory

**Packet 28 — Item Placement**

```c
case 28: {
    Packet28* pkt = (Packet28*)msg;
    // Host is telling us an item was moved
    int64_t item = obj_pool_perm_lookup(pkt->item_oid);
    int64_t container = obj_pool_perm_lookup(pkt->container_oid);
    if (item == OBJ_HANDLE_NULL || container == OBJ_HANDLE_NULL) {
        MP_WARN(MP_CAT_ITEM, "Packet28: item or container OID not found");
        break;
    }
    MP_DEBUG(MP_CAT_ITEM, "Item placement: item moved into container");
    item_insert(item, container, pkt->slot_idx);
    break;
}
```

**Packet 93 — Item Visibility**

```c
case 93: {
    Packet93* pkt = (Packet93*)msg;
    int64_t item = obj_pool_perm_lookup(pkt->oid);
    if (item == OBJ_HANDLE_NULL) break;
    if (pkt->field_20 == 0) {
        obj_set_flag(item, OIF_NO_DISPLAY);
    } else {
        obj_clear_flag(item, OIF_NO_DISPLAY);
    }
    MP_TRACE(MP_CAT_ITEM, "Item visibility: %s", pkt->field_20 ? "show" : "hide");
    break;
}
```

**Packet 98 — Player Flags**

```c
case 98: {
    PacketMultiplayerFlagsChange* pkt = (PacketMultiplayerFlagsChange*)msg;
    if (pkt->client_id >= NUM_PLAYERS) break;
    stru_5E8AD0[pkt->client_id].flags =
        (stru_5E8AD0[pkt->client_id].flags & ~0xFF00) | (pkt->flags & 0xFF00);
    MP_DEBUG(MP_CAT_SYNC, "Player %d flags updated: 0x%04X",
             pkt->client_id, pkt->flags);
    break;
}
```

#### Priority 5 — Spells & Effects

**Packet 72 — Object Destroy (used by summon cleanup)**
Already covered above.

**Packet 73 — Summon**

```c
case 73: {
    PacketSummon* pkt = (PacketSummon*)msg;
    MP_INFO(MP_CAT_SPELL, "Received summon: proto=%d at location",
            pkt->proto_id);
    int64_t creature = object_create_npc(pkt->proto_id, &pkt->location);
    if (creature == OBJ_HANDLE_NULL) {
        MP_ERROR(MP_CAT_SPELL, "Failed to create summoned creature");
        break;
    }
    // Set OID to match host's OID so future packets reference it correctly
    obj_set_id(creature, pkt->oid);
    break;
}
```

**Packet 76 — Damage Spell** (host executes, clients apply result)

```c
case 76: {
    PacketDamageSpell* pkt = (PacketDamageSpell*)msg;
    int64_t target = obj_pool_perm_lookup(pkt->target_oid);
    if (target == OBJ_HANDLE_NULL) {
        MP_WARN(MP_CAT_SPELL, "Packet76: target OID not found");
        break;
    }
    // Apply pre-calculated damage (host did the calculation)
    MP_DEBUG(MP_CAT_SPELL, "Applying spell damage: %d to target", pkt->damage);
    critter_apply_damage(target, pkt->damage, pkt->damage_type);
    break;
}
```

**Packet 77 — Eye Candy**

```c
case 77: {
    PacketEyeCandy* pkt = (PacketEyeCandy*)msg;
    MP_TRACE(MP_CAT_SPELL, "Playing eye candy effect art=%d", pkt->art_id);
    // Play the visual effect locally (no game state change)
    art_play_at(pkt->art_id, &pkt->location);
    break;
}
```

### 4.2 Logging in `multiplayer_handle_message()`

Add at the top of the switch:

```c
MP_LOG_RECV(*(int*)msg, /* size unknown here — add size param */ 0);
```

Add a default case that wasn't there before:

```c
default:
    MP_WARN(MP_CAT_PKT, "Unhandled packet type %d received", *(int*)msg);
    break;
```

**Deliverable**: All known packet types have handlers. Unknown packets are logged. Remote players' actions are visible in-game.

---

## Phase 5 — Bi-Directional Sync

**Goal**: The local player's actions are sent to the network. Currently almost nothing is sent from the local client.

**Files to modify**: `src/ui/intgame.c`, `src/game/multiplayer.c`, `src/game/mp_utils.h`

### 5.1 Local Player Movement → Packet 27

When the local player moves (position changes), send Packet27 to the host (if client) or broadcast to all clients (if host).

In `src/game/intgame.c`, after the movement goal is added (around line 3570), add:

```c
// If in multiplayer, send our new position to the network
if (tig_net_is_active()) {
    int64_t pc = player_get_local_pc_obj();
    TigPoint loc = obj_get_location(pc);
    mp_send_object_location(pc, loc);
    MP_TRACE(MP_CAT_SYNC, "Sent local player position");
}
```

`mp_send_object_location()` already exists in `mp_utils.h:697`. It builds and sends Packet27. No new code needed for the send side.

**Authority**: If client sends Packet27, host receives it, validates (is this a legal move?), applies it, and re-broadcasts to all other clients. Host's broadcast is what clients apply to show the moving player. The moving client shows its own position locally.

For MVP, skip the host validation step — host receives Packet27 from client and re-broadcasts it directly. Add validation later.

On the host side, add in `multiplayer_handle_message()` case 27, when `tig_net_is_host()`:

```c
case 27: {
    Packet27* pkt = (Packet27*)msg;
    // Apply locally (host updates its state)
    // ... existing handler code ...
    // Then re-broadcast to all OTHER clients
    MP_TRACE(MP_CAT_SYNC, "HOST: Re-broadcasting Packet27 to all clients");
    tig_net_send_app_all(pkt, sizeof(*pkt));  // This will need to broadcast except sender
    break;
}
```

Note: `tig_net_send_app_all` currently sends to all including the originator. This is acceptable for MVP (the client will re-apply its own position, which is idempotent). Phase 9 can add "send except" logic.

### 5.2 Combat Actions → Intent Packets

When the local player attacks (from `intgame.c` attack input), send an intent packet to the host. The host validates and executes combat, then broadcasts the damage result via Packet76.

This requires a new packet type: `PacketAttackIntent` (or reuse an existing subtype). For MVP, use an existing packet subtype if one matches. If not, define:

```c
// In mp_utils.h
typedef struct {
    int type;       // New type number (e.g., 200 to avoid conflicts)
    ObjectID attacker_oid;
    ObjectID target_oid;
    int weapon_slot;
} PacketAttackIntent;
```

Add to `net_compat.h` comment block that packet types 200+ are CE additions not in original Arcanum.

### 5.3 Spell Cast Intent

When local player casts a spell (`magictech.c` cast entry point), non-host clients currently do nothing. Add:

```c
// In magictech cast function, non-host path (currently just returns):
if (tig_net_is_active() && !tig_net_is_host()) {
    // Send spell intent to host
    PacketSpellIntent pkt;
    pkt.type = 201;  // CE addition
    pkt.caster_oid = obj_get_id(caster);
    pkt.target_oid = obj_get_id(target);
    pkt.spell_id = spell_id;
    tig_net_send_app_all(&pkt, sizeof(pkt));
    MP_DEBUG(MP_CAT_SPELL, "Client sent spell intent: spell=%d", spell_id);
    return true;
}
```

Host receives intent → executes spell → broadcasts result packets (Packet76 for damage, Packet77 for visual effect, Packet72 for summon destroy, etc.).

### 5.4 What NOT to Sync

Do not add sync for:
- Camera movement
- Hover/cursor state
- Map exploration fog (each player has their own)
- Dialog choices (each player's conversation is local)
- Inventory window position/size (local UI)

**Deliverable**: Local player moves, attacks, and casts are visible to other players. This is the "feels like multiplayer" milestone.

---

## Phase 6 — Character File Transfer

**Goal**: When a client joins, the host sends all player character files. When the client's character is ready, the host loads it into the world.

**Current situation**: `tig_net_xfer_*` stubs are no-op. Character files sit on the host's disk and never reach the client.

**Decision**: Implement file transfer as in-band packets rather than a separate xfer system. Send file data in chunks over the existing TCP connection as packets.

### 6.1 New Packet Types for File Transfer

```c
// CE addition: in-band file transfer
typedef struct {
    int      type;        // 202
    char     filename[64]; // Destination filename (relative to game dir)
    uint32_t file_size;   // Total file size in bytes
    uint32_t chunk_count; // Number of chunks to follow
} PacketFileTransferStart;

typedef struct {
    int      type;        // 203
    char     filename[64];
    uint32_t chunk_index;
    uint32_t chunk_size;
    uint8_t  data[8192];  // Chunk data
} PacketFileTransferChunk;

typedef struct {
    int  type;        // 204
    char filename[64];
} PacketFileTransferEnd;
```

### 6.2 Sender Side (Host)

In `multiplayer.c`, replace the `tig_net_xfer_send_as()` calls with:

```c
static void mp_send_file(const char* local_path, const char* dest_filename, int client_id)
{
    FILE* f = fopen(local_path, "rb");
    if (!f) {
        MP_ERROR(MP_CAT_SESSION, "Cannot open file for transfer: %s", local_path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Send start packet
    PacketFileTransferStart start_pkt = {0};
    start_pkt.type = 202;
    strncpy(start_pkt.filename, dest_filename, sizeof(start_pkt.filename) - 1);
    start_pkt.file_size = (uint32_t)file_size;
    start_pkt.chunk_count = (uint32_t)((file_size + 8191) / 8192);
    net_send_message_to(client_id, &start_pkt, sizeof(start_pkt));
    MP_INFO(MP_CAT_SESSION, "File transfer start: %s (%ld bytes, %u chunks)",
            dest_filename, file_size, start_pkt.chunk_count);

    // Send chunks
    PacketFileTransferChunk chunk_pkt;
    uint32_t chunk_idx = 0;
    while (!feof(f)) {
        chunk_pkt.type = 203;
        strncpy(chunk_pkt.filename, dest_filename, sizeof(chunk_pkt.filename) - 1);
        chunk_pkt.chunk_index = chunk_idx++;
        chunk_pkt.chunk_size = (uint32_t)fread(chunk_pkt.data, 1, sizeof(chunk_pkt.data), f);
        if (chunk_pkt.chunk_size == 0) break;
        net_send_message_to(client_id, &chunk_pkt,
                            offsetof(PacketFileTransferChunk, data) + chunk_pkt.chunk_size);
    }
    fclose(f);

    // Send end packet
    PacketFileTransferEnd end_pkt = {0};
    end_pkt.type = 204;
    strncpy(end_pkt.filename, dest_filename, sizeof(end_pkt.filename) - 1);
    net_send_message_to(client_id, &end_pkt, sizeof(end_pkt));
    MP_INFO(MP_CAT_SESSION, "File transfer complete: %s", dest_filename);
}
```

### 6.3 Receiver Side (Client Handler)

In `multiplayer_handle_message()`, add cases 202, 203, 204:

```c
case 202: { // File transfer start
    PacketFileTransferStart* pkt = (PacketFileTransferStart*)msg;
    MP_INFO(MP_CAT_SESSION, "Receiving file: %s (%u bytes)", pkt->filename, pkt->file_size);
    // Open a write handle (tracked in a static table by filename)
    mp_xfer_open_receive(pkt->filename, pkt->file_size);
    break;
}
case 203: { // File transfer chunk
    PacketFileTransferChunk* pkt = (PacketFileTransferChunk*)msg;
    mp_xfer_write_chunk(pkt->filename, pkt->data, pkt->chunk_size, pkt->chunk_index);
    break;
}
case 204: { // File transfer end
    PacketFileTransferEnd* pkt = (PacketFileTransferEnd*)msg;
    mp_xfer_finish(pkt->filename);
    MP_INFO(MP_CAT_SESSION, "File received: %s", pkt->filename);
    // Check if all expected files are received; if so, notify multiplayer system
    mp_xfer_check_all_complete();
    break;
}
```

### 6.4 Replace `tig_net_xfer_count()`

Once Phase 6 is implemented, `tig_net_xfer_count(client)` should return the number of in-flight file transfers for that client. Maintain a counter in `mp_xfer_state` and expose it.

### 6.5 Sequence

```
Client connects
  → Host receives NET_EVENT_CLIENT_CONNECTED
  → Host sends Packet0 (player list)
  → Host sends all Players\*.mpc files via Packet202/203/204
  → Host sends all portrait .bmp files via Packet202/203/204
  → Host sends Packet64 (map load)
Client receives Packet64
  → Client loads the map
  → Client spawns all received player objects
  → Multiplayer game begins
```

**Deliverable**: Client can join and receive all character files. Characters appear correctly in-world on both sides.

---

## Phase 7 — Clock Synchronization

**Goal**: Remove the 8× time catchup hack. Replace with a clock offset so client game time tracks host game time without acceleration.

**File to modify**: `src/game/timeevent.c`, `src/game/multiplayer.c`

### 7.1 The Fix

Add to `multiplayer.c`:

```c
static int32_t s_host_time_offset = 0;  // client_time + offset = host_time
```

When `PacketGameTime` (Packet1) is received on the client, compute the offset:

```c
case 1: {
    PacketGameTime* pkt = (PacketGameTime*)msg;
    if (!tig_net_is_host()) {
        int32_t offset = (int32_t)pkt->game_time - (int32_t)current_game_time;
        // Smooth the offset: don't jump, drift toward it
        s_host_time_offset += (offset - s_host_time_offset) / 4;
        MP_DEBUG(MP_CAT_TIME, "Clock sync: host=%d local=%d offset=%d smoothed=%d",
                 pkt->game_time, current_game_time, offset, s_host_time_offset);
    }
    break;
}
```

### 7.2 Remove the 8× Multiplier

In `timeevent.c:825-841`, remove the `delta_ms *= 8` block entirely. Replace with:

```c
// Apply host time offset if we're a client
if (tig_net_is_active() && !tig_net_is_host()) {
    // Offset is applied when comparing against host-sent timestamps,
    // not by accelerating local time. No multiplier needed.
}
game_time += delta_ms;  // Normal time advancement
```

### 7.3 Propagate Offset for Time Comparisons

Any code that compares local `game_time` against a host-sent timestamp should use `game_time + s_host_time_offset`. Expose:

```c
int32_t multiplayer_get_host_time_offset(void);
```

For MVP, smoothing isn't critical — just snap to host time when offset > 1000ms (1 real second):

```c
if (abs(offset) > 1000) {
    current_game_time = pkt->game_time;  // Hard snap
    s_host_time_offset = 0;
    MP_WARN(MP_CAT_TIME, "Hard snap to host time (offset was %d)", offset);
}
```

**Deliverable**: Game time stays synchronized without acceleration artifacts. Buff timers and animations play at normal speed.

---

## Phase 8 — Server Options & Configuration

**Goal**: PvP, friendly fire, auto-equip, and key sharing rules are configurable from the host UI and enforced in gameplay.

**Files to modify**: `src/ui/mainmenu_ui.c` (new host config window), `src/net/network.c` (`g_server_options`), `src/net_compat.h`

### 8.1 Host Configuration Window

Add a new main menu window `MM_WINDOW_HOST_CONFIG` that appears after the host chooses "Host Game" but before character selection. It contains:

- **Max players** (2–8 spinner)
- **PvP** checkbox
- **Friendly fire** checkbox
- **Auto-equip new joiners** checkbox
- **Key sharing** checkbox
- **Port** number (default 12345)

On OK, write values to `g_server_options`, `g_mp_max_players`, and `NET_PORT` (via a configurable global), then proceed to character selection.

### 8.2 Net Compat Fix

Replace `tig_net_local_server_get_options()` stub:

```c
#define tig_net_local_server_get_options() net_get_server_options()
```

`net_get_server_options()` returns `g_server_options`. Now all the game-code checks (combat rules, auto-equip, key sharing) actually work.

### 8.3 Max Players Enforcement

In `net_poll()` on the server, after `accept()`, check:

```c
if (client_count >= g_mp_max_players) {
    MP_WARN(MP_CAT_NET, "Rejecting client: max players (%d) reached", g_mp_max_players);
    closesocket_compat(new_sock);
    // Optionally send a rejection packet first
}
```

**Deliverable**: Server options are configurable and enforced. PvP and friendly fire rules work as designed.

---

## Phase 9 — Disconnection & Error Handling

**Goal**: Graceful handling of all failure modes without crashing or corrupting single-player state.

**Files to modify**: `src/game/multiplayer.c`, `src/ui/mainmenu_ui.c`, `src/net/network.c`

### 9.1 Client Disconnection (Host Side)

When `NET_EVENT_CLIENT_DISCONNECTED` fires in `multiplayer_handle_network_event()`:

```c
void multiplayer_handle_network_event(int event_type, int client_id)
{
    switch (event_type) {
    case NET_EVENT_CLIENT_CONNECTED:
        MP_INFO(MP_CAT_SESSION, "Client %d connected", client_id);
        // Begin join sequence: send player list, send character files, send map info
        mp_begin_client_join(client_id);
        break;

    case NET_EVENT_CLIENT_DISCONNECTED:
        MP_WARN(MP_CAT_SESSION, "Client %d disconnected", client_id);
        // Remove player object from world
        int64_t obj = multiplayer_get_obj_for_client(client_id);
        if (obj != OBJ_HANDLE_NULL) {
            object_destroy(obj);
            // Broadcast destruction to remaining clients
            PacketObjectDestroy pkt;
            pkt.type = 72;
            pkt.oid = obj_get_id(obj);
            tig_net_send_app_all(&pkt, sizeof(pkt));
        }
        // Free the slot
        stru_5E8AD0[client_id].flags = 0;
        clients[client_id].connected = false;
        break;

    case NET_EVENT_CONNECTION_LOST:
        MP_ERROR(MP_CAT_SESSION, "Connection to host lost");
        // Client: show error, return to main menu
        mp_connection_lost();
        break;
    }
}
```

### 9.2 Connection Error UI

Add a modal dialog shown when connection fails or is lost:
- "Connection to server lost. Returning to main menu."
- OK button → closes game session, returns to `MM_WINDOW_MAINMENU`, resets `g_game_mode`.

Use the existing dialog system. If no generic modal exists, add a simple one.

### 9.3 Timeout Detection

In `net_poll()`, track time since last receive per client. If > 30 seconds, treat as disconnected:

```c
// In ClientSlot struct:
time_t last_recv_time;

// After successful recv:
clients[i].last_recv_time = time(NULL);

// In each poll:
if (clients[i].connected && time(NULL) - clients[i].last_recv_time > 30) {
    MP_WARN(MP_CAT_NET, "Client %d timed out (30s no data)", i);
    disconnect_client(i);
}
```

Send keepalive packets every 5 seconds (a 1-byte packet type 255 that the handler silently ignores) to prevent false timeouts.

### 9.4 Prevent SP Corruption

Ensure `net_stop_server()` and `net_stop_client()` reset all state:
- `g_game_mode = GAME_MODE_SINGLE_PLAYER`
- `is_active = false`, `is_host = false`
- All client slots cleared
- All multiplayer globals reset to initial state
- Log session end

This ensures a single-player game started after a failed multiplayer session is clean.

---

## Testing Checkpoints

Each phase has a pass/fail test before moving to the next.

### Phase 0 Checkpoint
- Start game, check `arcanum_mp.log` exists
- Contains session header line
- Single player game works normally

### Phase 1 Checkpoint
- Start host, check log shows "Listening on port 12345"
- Connect with a TCP client (`telnet 127.0.0.1 12345` or `nc`)
- Log shows "Client connected"
- Disconnect: log shows "Client disconnected"
- Start two game instances, one host one client, both log connection

### Phase 2 Checkpoint
- In multiplayer mode, `tig_net_client_is_active(0)` returns true for local player slot
- Party iteration in debug context finds at least one player
- Log shows no more "always returns 0" warnings

### Phase 3 Checkpoint
- Host: click Host Game → log shows "Starting as HOST"
- Client: click Join Game → enter address → log shows "CLIENT: Attempting to connect"
- Both: game progresses past character creation
- Log shows both modes distinguishable

### Phase 4 Checkpoint
- Host moves an NPC: client sees it move (Packet27 handler)
- Host party changes: client party table updates (Packet71 handler)
- Unknown packet type: log shows WARN
- All 25+ packet types have handlers (even if stub with log entry)

### Phase 5 Checkpoint
- Client moves local player: host sees the player at new position
- Client attacks: host executes combat, damage appears on both screens
- No more silent sends that go nowhere

### Phase 6 Checkpoint
- Host has character file; client joins fresh (no character file locally)
- Log shows file transfer start/chunks/end
- Client's character appears correctly in-world after join

### Phase 7 Checkpoint
- Both clients have synchronized game time (check via debug display)
- No more animation speed fluctuations during join
- `timeevent.c` no longer contains 8× multiplier

### Phase 8 Checkpoint
- Host sets PvP disabled; players cannot damage each other
- Host sets max players 2; third connection is rejected (logged)
- Auto-equip works or doesn't based on setting

### Phase 9 Checkpoint
- Kill host process: client shows error dialog, returns to main menu cleanly
- Kill client process: host logs disconnect, game continues for host
- Rejoin after disconnect: works without stale state

---

## File Change Index

| File | Phase | Changes |
|------|-------|---------|
| `src/net/mp_log.h` | 0 | **NEW** — Logging API |
| `src/net/mp_log.c` | 0 | **NEW** — Logging implementation |
| `src/net/network.h` | 1, 2 | Add multi-client API, globals for server options and join address |
| `src/net/network.c` | 0, 1 | Remove `net_log()`, fix multi-client, fix TCP framing, fix bind address, use MP_* logging |
| `src/net_compat.h` | 2 | Fix `tig_net_client_is_active`, `tig_net_send_app`, `tig_net_start_client`, server options |
| `src/ui/mainmenu_ui.c` | 3, 8 | Add `g_game_mode`, fix button handlers, fix `sub_5412E0()`, add host config window |
| `src/game/multiplayer.c` | 4, 5, 6, 7, 9 | Implement all packet handlers, add `mp_send_file()`, add `s_host_time_offset`, fix network event handler |
| `src/game/timeevent.c` | 7 | Remove 8× catchup multiplier |
| `src/game/anim.c` | 5 | Remove non-host Packet9 zero broadcast; fix AnimID equality after Phases complete |
| `src/game/intgame.c` | 5 | Add local player position send on move |
| `src/game/magictech.c` | 5 | Add spell intent packet from non-host client |
| `src/game/mp_utils.h` | 5, 6 | Add new CE packet types (200+): intent packets, file transfer packets |
| `CMakeLists.txt` | 0 | Add `mp_log.c` to build |
