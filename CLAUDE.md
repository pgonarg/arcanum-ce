# Arcanum Multiplayer Integration - Research & Implementation Roadmap

## Project Goal
Hook up multiplayer functionality so that players can:
1. Select "Multiplayer" from main menu
2. Choose "Join Game" or "Host Game"
3. Create/select a character
4. Either connect to a server or start hosting one

---

## Critical Finding: Network Layer is Stubbed

**Location**: `src/net_compat.h`

All networking calls are **no-op macros**:
```c
#define tig_net_is_active() 0
#define tig_net_start_client() 0
#define tig_net_start_server()
#define tig_net_send_app_all(a, b)
```

**Implication**: The actual network transport layer is not included in the Community Edition. The infrastructure exists (message handlers, packet structures, player sync logic), but **messages cannot be sent over the network**.

---

## Game Initialization Flow

### Entry Point: `main()` in `src/main.c:66`
1. Parse command-line arguments
2. Initialize TIG engine
3. `gamelib_init(&game_init_info)` - Initialize game systems
4. `gamelib_mod_load()` - Load default module
5. `mainmenu_ui_handle()` - Show main menu
6. `main_loop()` - Game loop (line 330)

### GameInitInfo Structure (`src/game/context.h:19`)
```c
typedef struct GameInitInfo {
    bool editor;
    tig_window_handle_t iso_window_handle;
    IsoInvalidateRectFunc* invalidate_rect_func;
    IsoRedrawFunc* draw_func;
} GameInitInfo;
```
**Problem**: No field for multiplayer mode! Need to extend or create parallel state.

---

## Menu System & Character Selection

### Current Flow (Single Player)
```
MM_WINDOW_MAINMENU
  → "Single Player" button
    → MM_WINDOW_SINGLE_PLAYER
      → "New Character" button
        → MM_WINDOW_PICK_NEW_OR_PREGEN
          → MM_WINDOW_CHAREDIT
            → Character Editor
              → OK button calls sub_5412D0()
                → sub_5412E0(bool) starts game
```

### Multiplayer Flow (Needs Implementation)
```
MM_WINDOW_MAINMENU
  → "Multiplayer" button
    → MM_WINDOW_MULTIPLAYER (EXISTS BUT HAS NO FUNCTION!)
      → [NEW: Show Host vs Join options]
        → MM_WINDOW_PICK_NEW_OR_PREGEN
          → Character selection
            → sub_5412D0() [NEEDS MULTIPLAYER BRANCH]
```

### Key Function: `sub_5412E0()` in `src/ui/mainmenu_ui.c:1664`

This function handles game startup. It:
1. Gets local player object: `pc_obj = player_get_local_pc_obj()`
2. Loads starting map: `map = map_by_type(MAP_TYPE_START_MAP)`
3. Teleports player to start location
4. Closes menu and shows game UI

**What's Missing**: No branch for multiplayer! Needs to:
- Check game mode (single vs multiplayer)
- For multiplayer host: Call `sub_49CC50()` to start server
- For multiplayer client: Call `multiplayer_start()` to start client
- Set up network event handlers

---

## Multiplayer API

### Key Functions in `src/game/multiplayer.h`

| Function | Purpose | Status |
|----------|---------|--------|
| `multiplayer_init()` | Initialize multiplayer infrastructure | Implemented |
| `multiplayer_start()` | Start network client | Implemented (calls stubbed `tig_net_start_client()`) |
| `multiplayer_end()` | Stop networking | Implemented |
| `sub_49CC50()` | Start network server | Implemented (wrapper for `tig_net_start_server()`) |
| `sub_49CC70()` | Load multiplayer game from module | Implemented |
| `multiplayer_start_play()` | Create host player | Implemented |
| `multiplayer_handle_message()` | Process network packets | Implemented but never called |
| `multiplayer_handle_network_event()` | Handle connect/disconnect | Implemented but never called |

### Multiplayer Initialization Sequence

**In gamelib_init() (src/game/gamelib.c:379)**:
1. `multiplayer_init(init_info)` called early
2. Sets up TIG network infrastructure
3. But does NOT start networking

**When game starts (src/ui/mainmenu_ui.c:1664)**:
- Currently: Just loads starting map and teleports player
- Needed: Decide if host or client, then:
  - **Host mode**: `sub_49CC50()` → `tig_net_start_server()`
  - **Client mode**: `multiplayer_start()` → `tig_net_start_client()`

---

## Implementation Strategy

### Phase 1: Store Multiplayer Intent
1. Add game mode tracking (global or extended context)
2. Options: `SINGLE_PLAYER`, `MULTIPLAYER_HOST`, `MULTIPLAYER_JOIN`
3. Set by multiplayer menu when choosing Host/Join

### Phase 2: Implement Multiplayer Menu Window
1. **File**: `src/ui/mainmenu_ui.c`
2. **Add to mainmenu_ui_multiplayer_window_info**:
   - `init_func` - Initialize window (maybe add UI for host config)
   - `execute_func` - Handle button presses (Host/Join)
   - Change message base if needed

3. **Button structure**: Currently points to PICK_NEW_OR_PREGEN for both
   - This is CORRECT! Both host and join need to select character first
   - But we need to store the choice before going to character selection

### Phase 3: Branch Game Startup
1. **File**: `src/ui/mainmenu_ui.c:1664` (sub_5412E0)
2. **Add multiplayer startup code**:
   ```c
   if (game_mode == MULTIPLAYER_HOST) {
       sub_49CC50();  // Start server
   } else if (game_mode == MULTIPLAYER_JOIN) {
       multiplayer_start();  // Start client
   }
   ```
3. Keep existing single-player logic as default

### Phase 4: Network Address Input (Join Mode)
1. Add UI dialog for IP/hostname input
2. Pass address to multiplayer client initialization
3. May need new window type or dialog system

### Phase 5: Test & Debug
1. Start host, verify server starts
2. Start client, verify it attempts to connect
3. Handle connection failures gracefully

---

## Files Requiring Changes

### Priority 1 (Core Functionality)
- [ ] `src/ui/mainmenu_ui.c` - Implement multiplayer menu window init/execute
- [ ] `src/ui/mainmenu_ui.c` - Add game mode tracking (global variable)
- [ ] `src/ui/mainmenu_ui.c:1664` - Branch sub_5412E0 for multiplayer startup
- [ ] `src/game/context.h` - Consider extending GameInitInfo with mode field

### Priority 2 (UI/Polish)
- [ ] Add server address input dialog for join mode
- [ ] Add server configuration UI for host mode (optional for now)
- [ ] Connection status messages

### Priority 3 (Network - Requires External Implementation)
- [ ] `src/net_compat.h` - Implement actual network layer
  - Currently all calls are stubbed
  - Need real TCP/UDP socket implementation
  - This is a significant undertaking

---

## Testing Approach

1. **Host Mode**:
   - Start game → Multiplayer → Host Game → Create character → Game starts
   - Verify server listens (check with netstat)
   - Verify no crashes

2. **Client Mode**:
   - Start game → Multiplayer → Join Game → Create character → Attempt connect
   - Test with invalid IP (should fail gracefully)
   - Test with actual running host (requires network layer)

3. **Fallback Single Player**:
   - Ensure single player still works after changes

---

## Code Snippets & References

### Multiplayer Window Currently (src/mainmenu_ui.c:904)
```c
static MainMenuWindowInfo mainmenu_ui_multiplayer_window_info = {
    329,           // art_num (MainMenuBack.ART)
    NULL,          // init_func - NOT IMPLEMENTED!
    NULL,          // exit_func - NOT IMPLEMENTED!
    0,             // flags
    NULL,          // button_press_func
    NULL,          // button_release_func
    NULL,          // button_hover_func
    NULL,          // button_leave_func
    NULL,          // mouse_idle_func
    220,           // message_base (220=Join Game, 221=Server)
    SDL_arraysize(mainmenu_ui_multiplayer_buttons),
    mainmenu_ui_multiplayer_buttons,
    // ... rest of structure
};
```

### Current Multiplayer Buttons (src/mainmenu_ui.c:898)
```c
static MainMenuButtonInfo mainmenu_ui_multiplayer_buttons[] = {
    { 410, 143, -1, TIG_BUTTON_HANDLE_INVALID, MM_WINDOW_PICK_NEW_OR_PREGEN, 0, 0, { 0 }, -1 },  // Join → Char Select
    { 410, 193, -1, TIG_BUTTON_HANDLE_INVALID, MM_WINDOW_PICK_NEW_OR_PREGEN, 0, 0, { 0 }, -1 },  // Host → Char Select
    { 410, 243, -1, TIG_BUTTON_HANDLE_INVALID, -2, 0, 0x4, { 0 }, -1 },                          // Back (no message)
};
```

### Game Start Transition (src/ui/mainmenu_ui.c:1664)
```c
static void sub_5412E0(bool a1)
{
    if (mainmenu_ui_active) {
        gameuilib_wants_mainmenu_unset();
        pc_obj = player_get_local_pc_obj();
        
        if (mainmenu_ui_start_new_game) {
            // Load starting map and teleport
            map = map_by_type(MAP_TYPE_START_MAP);
            map_get_starting_location(map, &x, &y);
            
            // ... setup fade/teleport/movies ...
            
            teleport_do(&teleport_data);
            timeevent_add_delay(...);
        } else {
            if (dword_5C4004) sub_40FED0();
            gamelib_draw();
        }
        
        mainmenu_ui_close(false);
    }
    
    intgame_refresh_cursor();
    intgame_show();
}
```
**Action**: Add branch here for multiplayer startup

---

## Next Steps

1. Implement multiplayer menu window init/execute functions
2. Add game mode global variable
3. Add branching in sub_5412E0 for host/join startup
4. Test that buttons are clickable and mode is stored correctly
5. Test that game starts without crashing in multiplayer mode
6. Add network address input UI for join mode (optional for MVP)
