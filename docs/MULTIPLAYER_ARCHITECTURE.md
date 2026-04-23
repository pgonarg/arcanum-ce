# Arcanum CE — Multiplayer Architecture Deep Dive

> **Purpose**: Exhaustive technical analysis of the multiplayer system: what calls what, what's implemented, what's broken, where the coupling lies, and what would need to change to make it work.

---

## Table of Contents

1. [TL;DR — Why Is Multiplayer So Hard?](#1-tldr)
2. [System Initialization](#2-system-initialization)
3. [Network Layer](#3-network-layer)
4. [Packet System](#4-packet-system)
5. [Multiplayer Menu & UI Flow](#5-multiplayer-menu--ui-flow)
6. [Animation System Coupling](#6-animation-system-coupling)
7. [Magic & Spell System Coupling](#7-magic--spell-system-coupling)
8. [Item System Coupling](#8-item-system-coupling)
9. [Time Synchronization](#9-time-synchronization)
10. [Party System](#10-party-system)
11. [Other Engine Coupling](#11-other-engine-coupling)
12. [Character & File Management](#12-character--file-management)
13. [Global State & Architecture Problem](#13-global-state--architecture-problem)
14. [What Actually Works](#14-what-actually-works)
15. [Implementation Status Table](#15-implementation-status-table)
16. [What Needs to Change](#16-what-needs-to-change)

---

## 1. TL;DR

**The core problem is not a single missing piece — it is that multiplayer touches virtually every engine system.**

The codebase has a deeply-embedded host-authoritative multiplayer model from the original Arcanum (2001). The Community Edition has:

1. **Replaced the old no-op stubs** in `src/net_compat.h` with real mappings to a new `src/net/network.h` API.
2. **Not yet fully implemented** `src/net/network.c` — the actual socket/TCP layer.
3. **Left most packet handlers empty** — messages arrive but ~95% are silently discarded.
4. **Left bi-directional sync incomplete** — the game can receive some packets (Packet27 location) but only sends a handful.

Even when the network layer is complete, there are 150+ `tig_net_is_active()` / `tig_net_is_host()` checks spread across animation, magic, item, script, and UI code — all of which are part of the original design and need to work correctly together.

The architecture is salvageable. It is not broken — it is unfinished.

---

## 2. System Initialization

### 2.1 Module Registration

**`src/game/gamelib.c:169`** — Multiplayer is registered as a first-class module:

```c
{ "Multiplayer", multiplayer_init, multiplayer_reset, multiplayer_mod_load,
  multiplayer_mod_unload, multiplayer_exit, multiplayer_ping, NULL,
  multiplayer_save, multiplayer_load, NULL }
```

This means it participates in save/load, mod load/unload, and module ping cycles.

### 2.2 Initialization Call Graph

```
main()  [src/main.c:66]
  └─ gamelib_init(&game_init_info)  [src/game/gamelib.c]
       └─ multiplayer_init(init_info)  [src/game/multiplayer.c:360]
            ├─ Allocates 8 player slots (stru_5E8AD0[8])
            ├─ Initializes globals (dword_5F0E00 = false, etc.)
            └─ Returns true (always succeeds, no network contact yet)

... game runs in single-player mode ...

User clicks Host or Join in UI
  └─ [button handler in src/ui/mainmenu_ui.c]
       └─ sub_49CC50()  [host]  OR  multiplayer_start()  [client]
            │
            ├─ [HOST PATH]  sub_49CC50()  [src/game/multiplayer.c:645]
            │    └─ tig_net_start_server()  → net_start_server()
            │
            └─ [CLIENT PATH]  multiplayer_start()  [src/game/multiplayer.c:615]
                 ├─ multiplayer_lock_cnt = 0
                 ├─ tig_net_start_client()  → net_start_client("127.0.0.1")  ← HARDCODED!
                 ├─ tig_net_on_message(multiplayer_handle_message)
                 ├─ tig_net_on_message_validation(multiplayer_validate_message)
                 └─ tig_net_on_network_event(multiplayer_handle_network_event)
```

**Problem**: `net_start_client("127.0.0.1")` is hardcoded in `src/net_compat.h:19`. The address input UI exists but the value never reaches this call.

### 2.3 Game Loop Network Polling

**`src/main.c:388`** — `net_poll()` is called every frame before rendering. This is correct. If the network layer isn't running, this is a no-op.

---

## 3. Network Layer

### 3.1 The Three-Layer Stack

```
Game code (multiplayer.c, anim.c, item.c, etc.)
    calls tig_net_*() macros
          │
          ▼
src/net_compat.h   ← mapping layer
    #define tig_net_is_active()        net_is_active()
    #define tig_net_send_app_all(m,s)  net_send_message(m, s)
    etc.
          │
          ▼
src/net/network.h  ← modern API declaration
    net_start_server(), net_start_client(host)
    net_send_message(msg, size)
    net_poll()
    net_set_message_handler(handler)
    net_set_event_handler(handler)
          │
          ▼
src/net/network.c  ← IMPLEMENTATION (in progress)
```

### 3.2 What's Mapped (Working in Principle)

**`src/net_compat.h:16-22`**:

| Macro | Maps To | Status |
|-------|---------|--------|
| `tig_net_is_active()` | `net_is_active()` | Mapped |
| `tig_net_is_host()` | `net_is_host()` | Mapped |
| `tig_net_send_app_all(m,s)` | `net_send_message(m, s)` | Mapped |
| `tig_net_start_client()` | `net_start_client("127.0.0.1")` | Hardcoded IP! |
| `tig_net_start_server()` | `net_start_server()` | Mapped |
| `tig_net_on_message(h)` | `net_set_message_handler(h)` | Mapped |
| `tig_net_on_network_event(h)` | `net_set_event_handler(h)` | Mapped |

### 3.3 Critical No-Op Stubs That Break Core Logic

**`src/net_compat.h:24-51`** — These are the silent killers:

```c
#define tig_net_send_app(a, b, c)            // Send to single player — NO-OP
#define tig_net_send_app_except(a, b, c)     // Send to all except — NO-OP
#define tig_net_xfer_count(a)            0   // File xfer count — ALWAYS 0
#define tig_net_xfer_send(a, b, c)           // Send file — NO-OP
#define tig_net_xfer_send_as(a, b, c, d)     // Send file as — NO-OP
#define tig_net_client_is_active(a)      0   // Client active check — ALWAYS FALSE
#define tig_net_client_is_waiting(a)     0   // Client waiting — ALWAYS FALSE
#define tig_net_client_is_loading(a)     0   // Client loading — ALWAYS FALSE
#define tig_net_local_server_get_options() 0 // Server options — ALWAYS 0
#define tig_net_local_server_set_name(n)     // Set server name — NO-OP
#define tig_net_local_server_set_max_players(n) // Set max players — NO-OP
```

**Impact of each stub:**

- **`tig_net_client_is_active(a)` = 0**: The loop at `multiplayer.c:898-901` that finds connected clients NEVER finds anyone. Player detection is completely broken.
- **`tig_net_xfer_count(a)` = 0**: File sync at `multiplayer.c:872-876` waits for this to be 0. It's always 0, so the wait condition is always satisfied even though no files were transferred.
- **`tig_net_xfer_send_as()`** no-op: Character files (`.mpc`, `.bmp`) at `multiplayer.c:1547-1578` are "sent" but actually never go anywhere.
- **`tig_net_send_app()`** no-op: Targeted messages to specific players (as opposed to broadcast) never arrive.
- **`tig_net_local_server_get_options()` = 0**: All server config (PvP, friendly fire, auto-equip, key sharing) is always "off" regardless of what was configured.

### 3.4 Server Option Flags (Defined but Never Set)

**`src/net_compat.h:6-10`**:
```c
#define TIG_NET_SERVER_PLAYER_KILLING   0x0001
#define TIG_NET_SERVER_FRIENDLY_FIRE    0x0002
#define TIG_NET_SERVER_AUTO_EQUIP       0x0020
#define TIG_NET_SERVER_KEY_SHARING      0x0040
```

These are checked in `multiplayer.c:2615` (auto-equip), `multiplayer.c:3001-3070` (combat rules), and `item.c:1712` (key sharing) — but since `tig_net_local_server_get_options()` always returns 0, all rules are always "disabled."

---

## 4. Packet System

### 4.1 Packet Types Defined

All packet structs are in `src/game/mp_utils.h`. Each starts with an `int type` field used for dispatch.

| Type | Struct | Size | Purpose |
|------|--------|------|---------|
| 0 | `PacketGamePlayerList` | 0xC8 | Broadcast all player ObjectIDs to clients |
| 1 | `PacketGameTime` | 0x18 | Sync game_time and anim_time |
| 4 | `Packet4` | 0x28 | Object event (subtype, OID, location) |
| 5 | `Packet5` | 0x1A8 | Animation goal update (location, offsets, datetime) |
| 6 | `Packet6` | 0x78 | Spell/combat event (art_id, OIDs, location, spell_id) |
| 7 | `Packet7` | 0x198 | Animation goal restart |
| 8 | `Packet8` | 0x50 | Animation goal data modification |
| 9 | `Packet9` | 0x68 | Follower/animation state info |
| 10 | `Packet10` | 0x40 | Inventory slot update |
| 26 | `PacketCombatModeSet` | 0x28 | Enter/exit combat mode |
| 27 | `Packet27` | 0x28 | Object location update |
| 28 | `Packet28` | 0x40 | Item placement in inventory |
| 29 | `Packet29` | 0x20 | Unknown (OID only) |
| 46 | `Packet46` | 0x20 | Disconnect/exit game |
| 54 | `Packet54` | 0x08 | MagicTech ID |
| 64 | `Packet64` | 0x120 | Map load (player, map name) |
| 67 | `Packet67` | 0x08 | Field_4 only |
| 71 | `PacketPartyUpdate` | 0x24 | Full party array (8 slots) |
| 72 | `PacketObjectDestroy` | 0x20 | Object destruction |
| 73 | `PacketSummon` | varies | Summon creature |
| 75 | `PacketMagicTechObjFlag` | varies | Object flag change |
| 76 | `PacketDamageSpell` | varies | Damage spell execution |
| 77 | `PacketEyeCandy` | varies | Visual effects |
| 80 | `Packet80` | 0x40 | Item callback |
| 93 | `Packet93` | 0x28 | Item visibility toggle |
| 97 | `Packet97` | 0x38 | Object association |
| 98 | `PacketMultiplayerFlagsChange` | 0x0C | Player flags (always_run, auto_attack, etc.) |
| 100 | `Packet100` | varies | Item operations (subtype 12 = loot) |

Plus Packet99, Packet104, Packet118, Packet121-123, Packet128, and lag notification packets.

### 4.2 The Message Dispatcher

**`src/game/multiplayer.c:978-1070`** — `multiplayer_handle_message(void* msg)`:

```c
void multiplayer_handle_message(void* msg) {
    int type = *(int*)msg;
    switch (type) {
    case 0:   pkt0 = (PacketGamePlayerList*)msg;  /* NO HANDLER */ break;
    case 1:   pkt1 = (PacketGameTime*)msg;        /* NO HANDLER */ break;
    ...
    case 27:  pkt27 = (Packet27*)msg;
              // ONLY IMPLEMENTED HANDLER:
              if (pkt27->oid.type != OID_TYPE_NULL) {
                  int64_t obj = obj_pool_perm_lookup(pkt27->oid);
                  if (obj != OBJ_HANDLE_NULL)
                      sub_4A1F30(obj, pkt27->loc, 0, 0);
              }
              break;
    ...
    default:  break;  // Everything else silently discarded
    }
}
```

**Only Packet27 (location update) has a real handler.** All other packet types are cast to their struct type and then immediately fall out of scope — the data is never used.

### 4.3 What Sends Packets (Working)

These functions SEND packets successfully (assuming `net_send_message` is implemented):

| Function | File | Packet | Trigger |
|----------|------|--------|---------|
| `multiplayer_flags_set()` | multiplayer.c:2557 | Packet98 | Player flag change |
| `sub_4A50D0()` | multiplayer.c:2354 | Packet93 | Item hide |
| `sub_4A51C0()` | multiplayer.c:2393 | Packet93 | Item show |
| `sub_4A53B0()` | multiplayer.c:2484 | Packet97 | Object association |
| `sub_4A2040()` | multiplayer.c:1122 | Packet67 | Unknown |
| `multiplayer_notify_player_lagging()` | multiplayer.c:1162 | Lag packet | Lag detection |
| `party_update()` | party.c:267 | Packet71 | Party change |
| `anim_run_add_ex()` | anim.c:4000 | Packet9 | Animation start |
| Summon logic | magictech.c:1863 | PacketSummon/73 | Summon creature |
| Map load | multiplayer.c:882 | Packet64 | Map transition |

### 4.4 Packet Handlers Needed (Not Implemented)

For every packet that's sent, there must be a corresponding receive handler. Currently missing:

| Packet | What Handler Must Do |
|--------|---------------------|
| 0 (PlayerList) | Update `stru_5E8AD0` with all player ObjectIDs |
| 1 (GameTime) | Sync `game_time` and `anim_time` clocks |
| 4 (Object event) | Replay event on target object |
| 5 (Anim goal) | Apply animation goal to remote object |
| 6 (Spell event) | Play spell effect at location |
| 7 (Anim restart) | Restart animation on remote object |
| 8 (Anim modify) | Modify animation goal data |
| 9 (Follower) | Update follower animation state |
| 10 (Inventory) | Update item in inventory slot |
| 26 (Combat mode) | Set combat_mode flag on remote player |
| 28 (Item place) | Move item between containers |
| 29 (Unknown) | TBD |
| 46 (Disconnect) | Remove player from world, free slot |
| 64 (Map load) | Trigger client map transition |
| 71 (Party) | Apply party table update |
| 72 (Object destroy) | Remove object from scene |
| 73 (Summon) | Create summoned creature on client |
| 75 (ObjFlag) | Apply object flag change |
| 76 (DamageSpell) | Apply spell damage on client |
| 77 (EyeCandy) | Play visual effect |
| 80 (Item callback) | Execute item callback |
| 93 (Item visible) | Toggle `OIF_NO_DISPLAY` flag on item |
| 97 (Object assoc) | Associate objects (weapon-wield, etc.) |
| 98 (Flags) | Update remote player's flags |
| 100 (Item ops) | Execute item operation (loot, etc.) |

---

## 5. Multiplayer Menu & UI Flow

### 5.1 Window State Machine

**`src/ui/mainmenu_ui.c`**:

```
MM_WINDOW_MAINMENU
  ├─ "Single Player"  → MM_WINDOW_SINGLE_PLAYER
  └─ "Multiplayer"    → MM_WINDOW_MULTIPLAYER  [line 479]

MM_WINDOW_MULTIPLAYER  [lines 909-924]
  ├─ "Join Game"  → MM_WINDOW_PICK_NEW_OR_PREGEN  (button 0, line 899)
  ├─ "Host/Server" → MM_WINDOW_PICK_NEW_OR_PREGEN  (button 1, line 900)
  └─ "Back"       → back (button 2, line 901)

MM_WINDOW_PICK_NEW_OR_PREGEN
  └─ → character selection → sub_5412E0() → GAME STARTS
```

### 5.2 What Works in the Menu

- The "Multiplayer" button is visible when `tig_net_is_active()` is false (it always shows — this is intentional, it's a menu entry, not a guard). **Line 477-482**.
- The multiplayer window renders correctly.
- `mainmenu_ui_create_multiplayer()` — exists and sets up window. **Line 909**.
- `mainmenu_ui_multiplayer_button_released()` — exists and handles button presses. **Line 916**.
- An address input window exists: `mainmenu_ui_create_multiplayer_join_address()`. **Lines 943-967**.

### 5.3 What's Broken in the Menu

1. **Host button never calls `sub_49CC50()`**: Clicking "Host" goes straight to character selection without starting the server. The host role is never established.

2. **Join address is hardcoded**: `tig_net_start_client()` maps to `net_start_client("127.0.0.1")` in `net_compat.h:19`. The address input window's value is never passed through.

3. **No mode flag is stored**: When the user picks Host vs. Join, no global variable records the choice. By the time `sub_5412E0()` runs at game start, the choice is lost.

4. **`sub_5412E0()` has no multiplayer branch**: **`src/ui/mainmenu_ui.c:1664`** — this is where the game actually starts. It loads the starting map and teleports the player. There is no code here to:
   - Call `sub_49CC50()` if hosting
   - Call `multiplayer_start()` if joining
   - Set up network event handlers

5. **No connection feedback**: No "Connecting..." or "Connection failed" UI.

---

## 6. Animation System Coupling

This is the most pervasively coupled system. **`src/game/anim.c`** has over 120 network-aware branches.

### 6.1 Animation ID Comparison

**`src/game/anim.c:2806-2829`**:

```c
// In multiplayer, slot_num is IGNORED for equality checks
bool anim_id_run_is_equal(AnimID a, AnimID b) {
    if (tig_net_is_active())
        return a.unique_id == b.unique_id;  // slot_num ignored
    return a.unique_id == b.unique_id && a.slot_num == b.slot_num;
}
```

**Why**: In multiplayer, each client's animation system assigns its own local `slot_num`. Since these don't match across the network, equality must be determined by `unique_id` alone.

**Problem**: This weakens animation identity. Two different running animations can be considered the same, potentially causing wrong animation to be cancelled or modified.

### 6.2 Animation Delay (Non-Host Clients)

**`src/game/anim.c:3788-3792`**:

```c
// Host: random delay 0-300ms
// Non-host client: always 0 delay
if (!tig_net_is_active() || tig_net_is_host())
    delay = rand() % 300;
else
    delay = 0;
```

**Why**: Clients need to play animations immediately when they receive the sync packet from the host. Any artificial delay would make them visually lag further.

### 6.3 Animation Goal Execution (Followers)

**`src/game/anim.c:4211-4214`**:

```c
// Non-host clients: skip follower goal execution entirely
bool sub_444660(obj, goal) {
    if (tig_net_is_active() && !tig_net_is_host())
        return true;  // Early return — do nothing
    // ... actual follower goal processing ...
}
```

**Why**: Follower AI runs only on the host. Clients receive the resulting animation states via packets.

**Problem**: If the host doesn't send follower animation packets (and currently it mostly doesn't), followers appear frozen on clients.

### 6.4 Packet Broadcast on Animation Start

**`src/game/anim.c:4000-4025`**:

```c
void anim_run_add_ex(obj, goal, ...) {
    // ... set up animation goal ...
    
    if (tig_net_is_active() && tig_net_is_host()) {
        // Host: send full position and art data in Packet9
        pkt.pos = obj_get_pos(obj);
        pkt.art_id = obj_get_art(obj);
        tig_net_send_app_all(&pkt, sizeof(pkt));
    } else if (tig_net_is_active()) {
        // Non-host: send zeros (client doesn't know authoritative state)
        pkt.pos = {0};
        pkt.art_id = 0;
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}
```

**Problem**: Non-host clients broadcast Packet9 with zero data. The host would need to receive this, validate it, and re-broadcast the real values — but there's no Packet9 handler on the host.

### 6.5 Projectile Creation

**`src/game/anim.c:4270-4290`**:

```c
// Host: create object then broadcast via object creation packet
// Non-host: create locally with object_create_ex() (client-side predicted)
if (tig_net_is_host()) {
    projectile = object_create();
    // ... broadcast creation ...
} else {
    projectile = object_create_ex();  // Client-local, not synced
}
```

**Problem**: Projectiles on non-host clients are purely local. If the hit result doesn't match the host, there's no reconciliation.

### 6.6 Path Planning

**`src/game/anim.c:4814-4818`**:

```c
// Non-host: return immediately, skip pathfinding
bool sub_425BD0(obj, dest) {
    if (tig_net_is_active() && !tig_net_is_host())
        return true;  // Skip
    // ... actual A* pathfinding ...
}
```

**Why**: Pathfinding is host-authoritative. The host computes paths and broadcasts position updates (Packet27).

**Problem**: Non-host clients see players teleporting tile-by-tile instead of smoothly pathing, because they receive discrete position updates with no interpolation.

---

## 7. Magic & Spell System Coupling

**`src/game/magictech.c`** has 33+ network branches. The entire system is host-authoritative.

### 7.1 Non-Host Clients Cannot Execute Spells

**`src/game/magictech.c:1995-1997`**:

```c
bool magictech_effect_*(spell, obj, ...) {
    if (tig_net_is_active() && !tig_net_is_host())
        return true;  // Non-host: do nothing, wait for host
    // ... actual spell execution ...
}
```

This pattern repeats for **every spell effect function**. Non-host clients skip spell execution entirely.

### 7.2 Spell Damage is Host-Validated with Lock Check

**`src/game/magictech.c:1543-1556`**:

```c
void magictech_damage_spell(spell, target, ...) {
    if (tig_net_is_active() && !tig_net_is_host()) {
        if (multiplayer_is_locked())
            return;  // Locked: can't even request
        // Send Packet76 to host to execute damage
        tig_net_send_app_all(&pkt76, sizeof(pkt76));
        return;
    }
    // ... host executes damage directly ...
}
```

**Problem**: There's no Packet76 handler. The host receives the packet and discards it.

### 7.3 Summons Are Host-Only

**`src/game/magictech.c:1863-1879`**:

```c
void sub_450F50(summoner, spell) {
    if (tig_net_is_active() && !tig_net_is_host())
        return;  // Clients do nothing
    
    creature = object_create_npc(...);
    // ... set up creature ...
    
    if (tig_net_is_active()) {
        // Host broadcasts creature creation to all clients
        PacketSummon pkt;
        pkt.oid = obj_get_id(creature);
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}
```

**Problem**: PacketSummon handler is empty. Clients never create the summoned creature.

### 7.4 Eye Candy Effects (Host-Only)

**`src/game/magictech.c:2498-2518`**:

```c
void magictech_component_eye_candy(spell, ...) {
    if (tig_net_is_active() && !tig_net_is_host())
        return;  // Clients: no visual effects
    
    // ... play effect ...
    
    if (tig_net_is_active()) {
        // Broadcast Packet77 to all clients
        tig_net_send_app_all(&pkt77, sizeof(pkt77));
    }
}
```

**Problem**: Packet77 handler is empty. Clients see no spell visual effects.

---

## 8. Item System Coupling

**`src/game/item.c`** uses a proxy pattern: non-host clients cannot directly manipulate items, they send packets and wait.

### 8.1 Item Receive (Pickup)

**`src/game/item.c:618-629`**:

```c
bool item_receive_by_obj(item, recipient) {
    if (tig_net_is_active() && !tig_net_is_host()) {
        // Client: request pickup via Packet28
        Packet28 pkt;
        pkt.item_oid = obj_get_id(item);
        pkt.recipient_oid = obj_get_id(recipient);
        tig_net_send_app_all(&pkt, sizeof(pkt));
        return true;  // Optimistically return success
    }
    // Host/SP: actually move the item
}
```

**Problem**: Packet28 handler is empty. The host receives the request and discards it. The item never actually moves on the host, creating a desync.

### 8.2 Item Drop

**`src/game/item.c:713-722`**: Same pattern with Packet26 (not to be confused with `PacketCombatModeSet`, this is a different use of that type number — look carefully at the actual packet type field).

### 8.3 Key Sharing

**`src/game/item.c:1712-1722`**:

```c
bool item_key_has(party, key_proto) {
    if (tig_net_is_active() &&
        (tig_net_local_server_get_options() & TIG_NET_SERVER_KEY_SHARING)) {
        // Check all party members for the key
        for each player in party: check inventory
    }
    // SP: check only local player
}
```

**Problem**: `tig_net_local_server_get_options()` is always 0, so key sharing is always disabled.

### 8.4 Inventory Drag/Drop Lock

**`src/ui/inven_ui.c:2538-2540`**:

```c
if (tig_net_is_active() && !multiplayer_is_locked()) {
    sub_4A51C0();  // Show item (update visibility)
}
```

Item visibility updates are gated by `multiplayer_is_locked()`. The lock mechanism prevents inventory changes during certain network states.

---

## 9. Time Synchronization

### 9.1 The 8x Catchup Hack

**`src/game/timeevent.c:825-841`**:

```c
void timeevent_process_game_time(delta_ms) {
    if (tig_net_is_active() && !tig_net_is_host()) {
        // Non-host: run time at 8x speed when behind the host
        if (game_time < received_host_time)
            delta_ms *= 8;
    }
    game_time += delta_ms;
}
```

**Why**: When a client joins mid-game or falls behind due to lag, it needs to fast-forward to match the host's game time. The 8x multiplier is a rough catchup mechanism.

**Problem**: During catchup, all time-based events (animations, spell durations, buff timers, etc.) run 8x faster. This causes:
- Animation speed fluctuations
- Spell effects that flicker by
- Buff/debuff durations that expire too fast

### 9.2 Time Sync Packets

**`src/game/timeevent.c:903-914`**:

```c
void timeevent_process(delta_ms) {
    // Host: broadcast current game time every ~950ms
    static int sync_timer = 0;
    sync_timer += delta_ms;
    if (tig_net_is_active() && tig_net_is_host() && sync_timer > 950) {
        sync_timer = 0;
        PacketGameTime pkt;
        pkt.game_time = current_game_time;
        pkt.anim_time = current_anim_time;
        tig_net_send_app_all(&pkt, sizeof(pkt));
    }
}
```

**Problem**: Packet1 (GameTime) handler is empty. Clients never update their clocks from host broadcasts.

---

## 10. Party System

### 10.1 Two Completely Different Code Paths

**`src/game/party.c:95-144`**:

Single-player party iteration:
```c
obj_handle party_find_first() {
    return player_get_local_pc_obj();  // Returns the one PC
}
obj_handle party_find_next(obj) {
    return followers...  // Returns followers
}
```

Multiplayer party iteration:
```c
obj_handle party_find_first() {
    // Look up first active slot in dword_5FC32C[8] player slot array
    for (int i = 0; i < NUM_PLAYERS; i++) {
        if (tig_net_client_is_active(i))  // STUB: always returns 0!
            return dword_5FC32C[i];
    }
    return OBJ_HANDLE_NULL;
}
```

**Problem**: Since `tig_net_client_is_active()` always returns 0, the multiplayer party iterator NEVER returns any players. Party operations involving iteration are effectively broken.

### 10.2 Party Updates

**`src/game/party.c:267-287`**:

```c
void party_update() {
    if (tig_net_is_active() && tig_net_is_host()) {
        // Host: broadcast full party table every update
        PacketPartyUpdate pkt;
        memcpy(pkt.party, dword_5FC32C, sizeof(dword_5FC32C));
        tig_net_send_app_all(&pkt, sizeof(pkt));
    } else if (tig_net_is_active()) {
        // Non-host: receive and apply party updates (via Packet71 handler)
        // ... but Packet71 handler is empty ...
    }
}
```

---

## 11. Other Engine Coupling

### 11.1 Script Execution Gating

**`src/game/script.c:292-299`**:

```c
bool script_execute_trap(obj, script_idx) {
    if (tig_net_is_active() && !tig_net_is_host()) {
        // Non-host: only scripts at index 0 and 2 are allowed
        if (script_idx != 0 && script_idx != 2)
            return false;
    }
    // ...
}
```

Non-host clients have restricted script execution. Scripts at indices 1 and 3 are host-only.

**`src/game/script.c:2406-2460`**: Fade/teleport script actions (`SAT_FADE_AND_TELEPORT`, `SAT_FADE`) are disabled in multiplayer:

```c
case SAT_FADE_AND_TELEPORT:
    if (!tig_net_is_active())
        // Execute fade and teleport
    // Else: do nothing in MP
```

### 11.2 World Map UI

**`src/ui/wmap_ui.c:1251`**: In multiplayer, the world map closes automatically when accessed.

**`src/ui/wmap_ui.c:2192-2196`**: World map travel uses a different teleport call in MP:
```c
if (tig_net_is_active() && !tig_net_is_host())
    sub_433A00(dest, /*mp_flag=*/true);  // Client-side travel request
else
    sub_433A00(dest, /*mp_flag=*/false);  // Direct travel
```

### 11.3 Targeting

**`src/game/target.c:305-376`**: Only the host sets/clears the `CLICK_THROUGH` flag that makes party members non-clickable.

**`src/game/target.c:676-724`**: Party members cannot be targeted in multiplayer — special friend-check logic runs before allowing a target.

### 11.4 Turn-Based Combat Sector Changes

**`src/ui/tb_ui.c:345-352`**:

```c
void handle_sector_changed(obj) {
    if (tig_net_is_active())
        player = multiplayer_find_slot_from_obj(obj);  // MP: lookup via slot
    else
        player = player_is_local_pc_obj(obj) ? 0 : -1;  // SP: direct check
}
```

Turn-based combat sector notification uses completely different object identification in MP vs SP.

### 11.5 Input/Movement

**`src/ui/intgame.c:3552-3572`**: Movement in multiplayer sets special animation flags (0x100, 0x40, 0x4000) not set in single-player. After adding the animation goal, non-host clients return immediately rather than continuing movement logic.

### 11.6 `player_get_local_pc_obj()` Usage

This is called in many places throughout the codebase. In single-player it unambiguously returns the one player character. In multiplayer it still works correctly (returns local player's object), but many calling sites don't account for the fact that other players also exist:

- `src/ui/mainmenu_ui.c:1664` — `sub_5412E0()` uses it to get the player for map teleport; in multiplayer, ALL players need to be teleported.
- `src/game/anim.c` — multiple sites use local PC object as "the" player for animation decisions.
- `src/game/combat.c` — local PC-specific combat checks that should be per-player in MP.

---

## 12. Character & File Management

### 12.1 Character File Format

Characters are saved as `Players\{ObjectID}.mpc` with portraits at `{ObjectID}.bmp` and `{ObjectID}_b.bmp`.

### 12.2 Save/Load (Works)

- **`save_char(path, obj)`** — `src/game/multiplayer.c:1674-1789`: Serializes player object to disk. Validates: no inventory, no followers, no active effects. **Works.**
- **`load_char(path, &obj)`** — `src/game/multiplayer.c:1867-1923`: Deserializes from disk. **Works.**

### 12.3 File Transfer (Broken)

**`src/game/multiplayer.c:1547-1578`**: Host is supposed to send character files to joining clients:

```c
// For each other active player:
tig_net_xfer_send_as(src_path, dst_path, player_id, callback);  // NO-OP stub
// For portraits:
tig_net_xfer_send_as(portrait_path, dst_path, player_id, callback);  // NO-OP stub
```

**All file transfer functions are no-op stubs.** Characters never transfer between host and clients.

### 12.4 Character Sync Time Event

**`src/game/multiplayer.c:861-931`** — The time event handler for multiplayer async operations:

| `param[0]` | Purpose | Status |
|------------|---------|--------|
| 2 | Wait for file xfer completion, then send Packet64 (map load) | Broken: xfer_count always 0 |
| 3 | Disconnect cleanup: save char, call callback, reset | Works |
| 4 | Broadcast files to joining player | Broken: xfer_send is no-op |
| 5 | Unknown player operation | Unknown |

---

## 13. Global State & Architecture Problem

### 13.1 The Two Global Booleans That Control Everything

```c
// src/net/network.c
static bool is_active = false;
static bool is_host   = false;
```

These two booleans, accessed via `tig_net_is_active()` and `tig_net_is_host()`, control 150+ branching points across the entire engine. This is the original Arcanum design — it's not "wrong" per se, but it means:

1. **You cannot test MP code paths without a real network connection.** Every MP branch is gated by `is_active`.
2. **Single-player and multiplayer share the same code.** There's no separation of concerns — a bug in one can affect the other.
3. **Adding new multiplayer behavior requires touching every affected system.** There's no centralized multiplayer abstraction.

### 13.2 The Host-Authoritative Model

The entire design assumes:
- **Host** = authoritative source of truth for all game state
- **Clients** = read-only replicas that request actions via packets

This is a valid architecture (used by many games), but it requires:
1. The host to handle ALL client action requests
2. The host to broadcast ALL state changes to ALL clients
3. Clients to apply received state without second-guessing it

Currently, **step 2 is the bottleneck**: packets are sent but no handlers exist to apply them on clients.

### 13.3 Missing Pieces That Block Everything Else

In dependency order:

```
1. net_start_server() / net_start_client() must work
        ↓ depends on
2. TCP socket implementation in network.c
        ↓ then
3. Host must be set before game starts (UI fix in mainmenu_ui.c)
        ↓ then
4. tig_net_client_is_active() must return real values
        ↓ which enables
5. Party iteration to find connected players
        ↓ which enables
6. Player slot management to work correctly
        ↓ then
7. Packet handlers need implementing (25+ types)
        ↓ which enables
8. Remote players to animate, cast spells, use items
        ↓ then
9. File transfer (tig_net_xfer_*) for character sync
        ↓ which enables
10. Clients to load other players' characters
```

---

## 14. What Actually Works

Despite the above, these pieces ARE implemented and functional:

| Feature | Status | Notes |
|---------|--------|-------|
| Multiplayer module init | ✅ Works | 8 player slots allocated, globals set |
| Multiplayer menu UI | ✅ Renders | Buttons visible, window opens |
| `save_char()` / `load_char()` | ✅ Works | Files correctly serialized/deserialized |
| Packet27 location receive | ✅ Works | `sub_4A1F30()` applies received location |
| Player flags (send) | ✅ Works | Packet98 correctly constructed and sent |
| Item visibility (send) | ✅ Works | Packet93 correctly constructed and sent |
| Party update (send) | ✅ Works | Packet71 broadcast by host |
| Map load (send) | ✅ Works | Packet64 sent after file xfer |
| Lag notification (send) | ✅ Works | Lag/recovery packets sent |
| `net_poll()` in game loop | ✅ Integrated | Called every frame at `src/main.c:388` |
| Animation delay for clients | ✅ Logic | `anim.c:3788` correctly sets 0 delay for clients |
| Script execution gating | ✅ Logic | Correctly restricts client script access |
| World map MP behavior | ✅ Logic | Auto-closes, uses MP travel path |
| `multiplayer_is_locked()` | ✅ Works | Lock counter correctly managed |

---

## 15. Implementation Status Table

| Component | File(s) | Status | Blocking Issue |
|-----------|---------|--------|----------------|
| TCP socket layer | src/net/network.c | 🔴 Incomplete | No socket implementation |
| Client address input | mainmenu_ui.c, net_compat.h | 🔴 Broken | Hardcoded to 127.0.0.1 |
| Host/Join mode flag | mainmenu_ui.c | 🔴 Missing | No global stores the choice |
| Server start from UI | mainmenu_ui.c | 🔴 Missing | `sub_49CC50()` never called |
| Client start from UI | mainmenu_ui.c | 🔴 Missing | `multiplayer_start()` never called from `sub_5412E0()` |
| Packet handlers (all) | multiplayer.c:978 | 🔴 Missing | Only Packet27 implemented |
| `tig_net_client_is_active()` | net_compat.h | 🔴 Stubbed | Always returns 0 |
| `tig_net_xfer_*()` | net_compat.h | 🔴 Stubbed | File transfer completely non-functional |
| `tig_net_send_app()` | net_compat.h | 🔴 Stubbed | Targeted sends never delivered |
| Server options | net_compat.h | 🔴 Stubbed | Options always 0 |
| Bi-directional sync | multiplayer.c | 🔴 Missing | Local movement never sent |
| Packet27 send | multiplayer.c | 🟡 Partial | Function exists, not wired to movement |
| Animation sync (recv) | anim.c | 🔴 Missing | Packet9 handler empty |
| Spell sync (recv) | magictech.c | 🔴 Missing | Packet73/76/77 handlers empty |
| Item sync (recv) | item.c | 🔴 Missing | Packet28/100 handlers empty |
| Party sync (recv) | party.c | 🔴 Missing | Packet71 handler empty |
| Time sync (recv) | timeevent.c | 🔴 Missing | Packet1 handler empty |
| Character file transfer | multiplayer.c | 🔴 Broken | Depends on xfer stubs |
| Server config UI | mainmenu_ui.c | 🔴 Missing | No UI for host options |
| Player management | multiplayer.c | 🟡 Partial | Slots allocated, detection broken |
| Multiplayer menu render | mainmenu_ui.c | 🟢 Works | Displays correctly |
| save_char / load_char | multiplayer.c | 🟢 Works | Correctly implemented |
| Packet send (broadcast) | multiplayer.c | 🟢 Works | Assuming network.c implemented |
| net_poll() integration | main.c | 🟢 Works | Called every frame |
| Animation delay logic | anim.c | 🟢 Works | Client delay = 0 correct |
| `multiplayer_is_locked()` | multiplayer.c | 🟢 Works | Correctly managed |

---

## 16. What Needs to Change

Listed in the order that unblocks subsequent work:

### Step 1 — Complete `src/net/network.c`
Implement the TCP socket layer: server listen/accept, client connect, `net_send_message()`, `net_poll()`, event dispatch. This is prerequisite for everything.

### Step 2 — Fix Client IP Address
Change `net_compat.h:19` from `net_start_client("127.0.0.1")` to accept the address from the UI. Wire the join address input window's value through to `multiplayer_start()`.

### Step 3 — Fix the Menu Flow
In `src/ui/mainmenu_ui.c`:
1. Add a global `int current_game_mode` (SINGLE_PLAYER / MULTIPLAYER_HOST / MULTIPLAYER_JOIN).
2. Set it in `mainmenu_ui_multiplayer_button_released()` when Host or Join is clicked.
3. In `sub_5412E0()` at line 1664, add:
   ```c
   if (current_game_mode == MULTIPLAYER_HOST)   sub_49CC50();
   else if (current_game_mode == MULTIPLAYER_JOIN) multiplayer_start();
   ```

### Step 4 — Implement `tig_net_client_is_active()`
Replace the stub in `net_compat.h` with a real call that checks whether a given player slot ID has an active connection. This unblocks party iteration and player detection throughout the codebase.

### Step 5 — Implement Packet Handlers
In `multiplayer_handle_message()` (`multiplayer.c:978`), add handlers for all 25+ packet types. Start with the highest-impact ones:
- Packet0 (player list) — needed for initial join
- Packet1 (time sync) — needed for clock alignment
- Packet64 (map load) — needed for world transitions
- Packet71 (party) — needed for party system
- Packet5/7/9 (animations) — needed for visible movement
- Packet26 (combat mode) — needed for combat
- Packet46 (disconnect) — needed for graceful leave

### Step 6 — Wire Bi-Directional Position Sync
Find where local player position changes and call `mp_send_object_location()`. The receiving side (Packet27 handler) already works.

### Step 7 — Implement File Transfer
Replace `tig_net_xfer_*()` stubs with real file-over-TCP implementation. This enables character synchronization when players join.

### Step 8 — Implement `tig_net_local_server_get_options()`
Replace the stub with actual server option storage so PvP, friendly fire, auto-equip, and key sharing rules can be set from UI and respected by game code.

### Step 9 — Server Configuration UI
Add host configuration dialog (max players, PvP rules, etc.) before the game starts.

### Step 10 — Testing & Edge Cases
- Disconnection handling (Packet46 + cleanup)
- Reconnection support (or graceful rejection)
- Late join (send full game state to joining client)
- Host migration (if host disconnects)
