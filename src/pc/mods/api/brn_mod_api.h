/*
 * brn_mod_api.h -- the interface between Burnout_PC.exe and the WebAssembly mods in mods/<name>/.
 *
 * FLAG PC divergence: there is no console counterpart. This header is the only file a mod shares
 * with the game. Plain C, no game type, no pointer into game memory, no third-party library: a mod
 * is one .wasm file, the same for every OS and CPU, written in any language that targets
 * wasm32 (C/C++ with wasi-sdk or clang, Rust, Zig, AssemblyScript, TinyGo, ...).
 *
 * A mod is a folder mods/<name>/ holding mod.ini:
 *     [Mod]
 *     Name=Debug menu
 *     Wasm=debug_menu.wasm
 *     Version=0.1
 * and that module. It EXPORTS (see "the mod" below):
 *     memory                                   its linear memory (every toolchain exports it)
 *     uint32_t brn_mod_api_version(void)       returns BRN_MOD_API_VERSION
 *     int32_t  brn_mod_init(void)              non-zero = loaded
 *   and optionally brn_mod_tick, brn_mod_render, brn_mod_on_input + brn_mod_input_buffer,
 *   brn_mod_shutdown. It IMPORTS only the host functions below, grouped in versioned wasm import
 *   modules ("brn.core.v1", "brn.player.v1", ...). A module never changes once published: a
 *   different shape gets a new name ("brn.player.v2") and the old one stays. A mod that imports a
 *   function this build does not have still loads; that import traps if it is ever called, so
 *   test brn_has_module() first when you target several game versions.
 *
 * The sandbox: the mod sees its own linear memory and these imports, nothing else -- no WASI, no
 * file system, no network, no game pointer. Data is copied into the mod's memory; game objects
 * are named by index or id. Every pointer/length a mod passes is checked against its memory: an
 * out-of-bounds one traps. A trap (a crash, a Rust panic, running out of time or memory) switches
 * that mod off and names it in BrnGame.log; the game keeps running.
 *
 * Threads: brn_mod_on_input and brn_mod_render run on the render thread, brn_mod_tick on the
 * update thread, never at the same time (the host serialises one mod's calls). Getters return a
 * copy taken at the last game tick; commands are queued and applied at the next one.
 */
#ifndef BRN_MOD_API_H
#define BRN_MOD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRN_MOD_API_VERSION 2u

/* ======================================================================================== */
/* Data. Plain, pointer-free structs: identical layout in the game (x64) and in a mod (wasm32). */
/* ======================================================================================== */

typedef struct BrnVec3 { float x, y, z; } BrnVec3;

typedef struct BrnModInfo
{
    char name[64];
    char folder[64];
    char version[32];
    int  loaded;              /* the module is instantiated */
    int  enabled;             /* its exports run */
    char error[128];          /* why it is not loaded, when it is not */
} BrnModInfo;

typedef struct BrnPlayerState
{
    uint32_t size;
    int      valid;               /* 0 until a player car exists */
    BrnVec3  position;            /* world metres, y up */
    BrnVec3  direction;           /* unit forward */
    BrnVec3  velocity;            /* m/s */
    float    speed_mph;
    float    max_speed_mph;
    float    max_boost_speed_mph;
    float    rpm;
    int      gear;
    float    time_in_air;         /* seconds, current jump */
    float    time_drifting;
    float    time_boosting;
    float    time_crashing;
    int      crashing;
    int      driveable;
    uint64_t car_id;              /* CgsID */
    char     car_id_text[16];     /* CgsID as text, e.g. "PUSMC01" */
    char     car_name[64];
    uint64_t wheel_id;
    int      in_game_mode;        /* an event is running: car change is refused */
    int      game_mode_type;      /* GameStateModuleIO::EGameModeType */
    int      sim_paused;
} BrnPlayerState;

typedef struct BrnVehicleInfo
{
    uint64_t id;
    uint64_t parent_id;           /* 0 for a base car, else the car this is a version of */
    char     id_text[16];
    char     name[64];
    char     default_wheel[64];
    int      car_type;
    int      speed_stat, boost_stat, strength_stat;
} BrnVehicleInfo;

typedef struct BrnLocation
{
    char    label[96];            /* "Gas Station in Downtown, Paradise City" */
    int     type;                 /* BrnTrigger::GenericRegion::Type */
    char    type_name[32];
    BrnVec3 position;             /* in front of the region, as the debug menu spawns */
    BrnVec3 direction;
} BrnLocation;

typedef struct BrnRaceCarInfo
{
    int      active;
    int      is_player;
    BrnVec3  position;
    BrnVec3  direction;
    BrnVec3  velocity;
    float    speed_mph;
    int      crashing;
    uint64_t car_id;
    char     car_id_text[16];
} BrnRaceCarInfo;

typedef struct BrnTrafficSettings
{
    uint32_t size;
    int   turn_off;
    int   stop_moving;
    int   allow_anarchy;
    int   enable_avoidance;
    float speed_multiplier;       /* 0..50 */
    float density_scale;          /* game mode density scale, 0..16 */
    int   big_vehicle_amount;     /* 0..100 */
    float final_density;          /* read only */
} BrnTrafficSettings;

#define BRN_DEBUGVAR_MENU     0   /* a folder */
#define BRN_DEBUGVAR_FUNCTION 1   /* an action, or a component section that opens its controls */
#define BRN_DEBUGVAR_BOOL     2
#define BRN_DEBUGVAR_INT      3
#define BRN_DEBUGVAR_UINT     4
#define BRN_DEBUGVAR_FLOAT    5
#define BRN_DEBUGVAR_CHOICE   6   /* an int with named options */
#define BRN_DEBUGVAR_SECTION  7   /* a component's section: call() opens it and lists its controls;
                                     value 1 once it is open. Never an action with side effects. */

typedef struct BrnDebugVar
{
    char   path[192];             /* "Traffic/Turn traffic off" */
    char   name[64];              /* the last path segment */
    int    depth;                 /* 0 = top-level folder */
    int    kind;                  /* BRN_DEBUGVAR_* */
    int    read_only;
    int    has_range;
    double value;                 /* bool 0/1, int, uint, float, choice value */
    double min, max, step;        /* step 0 = none set */
    char   text[64];              /* the value as the game prints it */
} BrnDebugVar;

/* ---- input: what the host makes of the window's messages --------------------------------- */

#define BRN_INPUT_MOUSE_MOVE   1  /* x, y */
#define BRN_INPUT_MOUSE_BUTTON 2  /* code = BRN_MOUSE_*, pressed, x, y */
#define BRN_INPUT_MOUSE_WHEEL  3  /* x, y = notches (+y away from the user, +x right) */
#define BRN_INPUT_MOUSE_LEAVE  4
#define BRN_INPUT_KEY          5  /* code = BRN_KEY_*, pressed, repeat */
#define BRN_INPUT_TEXT         6  /* code = one Unicode code point */
#define BRN_INPUT_FOCUS        7  /* pressed = 1 gained, 0 lost */
#define BRN_INPUT_RESIZE       8  /* x, y = new back buffer size in pixels */

#define BRN_MOUSE_LEFT   0
#define BRN_MOUSE_RIGHT  1
#define BRN_MOUSE_MIDDLE 2
#define BRN_MOUSE_X1     3
#define BRN_MOUSE_X2     4

#define BRN_MODIFIER_CTRL  1u
#define BRN_MODIFIER_SHIFT 2u
#define BRN_MODIFIER_ALT   4u
#define BRN_MODIFIER_SUPER 8u

/* Keys: printable ones are their upper-case ASCII ('A'..'Z', '0'..'9', ' ', and the
   punctuation of a US layout); the others start at 256. */
enum
{
    BRN_KEY_UNKNOWN = 0,
    BRN_KEY_SPACE = ' ', BRN_KEY_APOSTROPHE = '\'', BRN_KEY_COMMA = ',', BRN_KEY_MINUS = '-',
    BRN_KEY_PERIOD = '.', BRN_KEY_SLASH = '/', BRN_KEY_SEMICOLON = ';', BRN_KEY_EQUAL = '=',
    BRN_KEY_LEFT_BRACKET = '[', BRN_KEY_BACKSLASH = '\\', BRN_KEY_RIGHT_BRACKET = ']',
    BRN_KEY_GRAVE = '`',
    BRN_KEY_ESCAPE = 256, BRN_KEY_ENTER, BRN_KEY_TAB, BRN_KEY_BACKSPACE, BRN_KEY_INSERT,
    BRN_KEY_DELETE, BRN_KEY_RIGHT, BRN_KEY_LEFT, BRN_KEY_DOWN, BRN_KEY_UP, BRN_KEY_PAGE_UP,
    BRN_KEY_PAGE_DOWN, BRN_KEY_HOME, BRN_KEY_END, BRN_KEY_CAPS_LOCK, BRN_KEY_SCROLL_LOCK,
    BRN_KEY_NUM_LOCK, BRN_KEY_PRINT_SCREEN, BRN_KEY_PAUSE,
    BRN_KEY_F1 = 290, BRN_KEY_F2, BRN_KEY_F3, BRN_KEY_F4, BRN_KEY_F5, BRN_KEY_F6, BRN_KEY_F7,
    BRN_KEY_F8, BRN_KEY_F9, BRN_KEY_F10, BRN_KEY_F11, BRN_KEY_F12,
    BRN_KEY_KP_0 = 320, BRN_KEY_KP_1, BRN_KEY_KP_2, BRN_KEY_KP_3, BRN_KEY_KP_4, BRN_KEY_KP_5,
    BRN_KEY_KP_6, BRN_KEY_KP_7, BRN_KEY_KP_8, BRN_KEY_KP_9, BRN_KEY_KP_DECIMAL, BRN_KEY_KP_DIVIDE,
    BRN_KEY_KP_MULTIPLY, BRN_KEY_KP_SUBTRACT, BRN_KEY_KP_ADD, BRN_KEY_KP_ENTER,
    BRN_KEY_LEFT_SHIFT = 340, BRN_KEY_LEFT_CTRL, BRN_KEY_LEFT_ALT, BRN_KEY_LEFT_SUPER,
    BRN_KEY_RIGHT_SHIFT, BRN_KEY_RIGHT_CTRL, BRN_KEY_RIGHT_ALT, BRN_KEY_RIGHT_SUPER, BRN_KEY_MENU
};

typedef struct brn_input_event
{
    uint32_t type;        /* BRN_INPUT_* */
    uint32_t modifiers;   /* BRN_MODIFIER_* held when the window received it */
    int32_t  code;        /* BRN_KEY_*, BRN_MOUSE_* or a code point */
    int32_t  pressed;     /* key / button down, focus gained */
    int32_t  repeat;      /* a held key auto-repeating */
    float    x, y;        /* back buffer pixels (mouse), notches (wheel), pixels (resize) */
} brn_input_event;

#define BRN_INPUT_BUFFER_CAPACITY 64

/* ---- UI: textured triangle lists, the shape ImGui (ImDrawData) and egui (ClippedPrimitive)
        both produce. The host draws them over the finished frame, in submission order. ------- */

typedef struct brn_vertex
{
    float    x, y;        /* back buffer pixels, origin top-left */
    float    u, v;
    uint32_t rgba;        /* bytes r, g, b, a in memory: ImDrawVert::col and egui Color32 as-is */
} brn_vertex;             /* 20 bytes: ImDrawVert and egui::epaint::Vertex have this layout */

typedef struct brn_rect { float x0, y0, x1, y1; } brn_rect;   /* clip rect, back buffer pixels */

#define BRN_UI_PREMULTIPLIED 1u   /* colours and texture are premultiplied by alpha (egui) */
#define BRN_UI_TEXTURE_LIMIT 8192 /* largest width / height of a texture */

#define BRN_CAPTURE_KEYBOARD 1u   /* the game stops reading the keyboard */
#define BRN_CAPTURE_MOUSE    2u
#define BRN_CAPTURE_GAMEPAD  4u   /* the game stops reading the pads too */

/* ======================================================================================== */
/* Host imports. Only compiled for a wasm target; the game includes this file for the data.  */
/* ======================================================================================== */

#if defined(__wasm__)

#define BRN_IMPORT(module, name) __attribute__((import_module(module), import_name(name)))
#define BRN_EXPORT(name) __attribute__((export_name(name)))

/* ---- "brn.core.v1" ---------------------------------------------------------------------- */
BRN_IMPORT("brn.core.v1", "log")             void     brn_log_n(const char* text, uint32_t len);
BRN_IMPORT("brn.core.v1", "has_module")      int32_t  brn_has_module_n(const char* name, uint32_t len);
BRN_IMPORT("brn.core.v1", "time")            double   brn_time(void);   /* seconds since the game started */
BRN_IMPORT("brn.core.v1", "set_input_capture") void   brn_set_input_capture(uint32_t flags);   /* BRN_CAPTURE_* */
BRN_IMPORT("brn.core.v1", "mod_count")       int32_t  brn_mod_count(void);
BRN_IMPORT("brn.core.v1", "get_mod_info")    int32_t  brn_get_mod_info(int32_t index, BrnModInfo* out);
/* Saved in mods/mods.ini. Applied before the next frame: switching a wasm mod on (re)loads its
   .wasm from disk, so it doubles as a reload. */
BRN_IMPORT("brn.core.v1", "set_mod_enabled") void     brn_set_mod_enabled(int32_t index, int32_t enabled);

/* ---- "brn.player.v1" -------------------------------------------------------------------- */
BRN_IMPORT("brn.player.v1", "get_state")     int32_t  brn_player_get_state(BrnPlayerState* out);   /* returns valid */
/* Same car, new pose (the game's debug teleport). A zero direction keeps the heading. */
BRN_IMPORT("brn.player.v1", "teleport")      void     brn_player_teleport(float x, float y, float z, float dx, float dy, float dz);
/* The game's "put me back on the road" reset, driving at speed_mph. */
BRN_IMPORT("brn.player.v1", "reset_on_track") void    brn_player_reset_on_track(float speed_mph);
/* Drop the car at a pose already moving forward at speed_mph (place-on-track). */
BRN_IMPORT("brn.player.v1", "place_on_track") void    brn_player_place_on_track(float x, float y, float z, float dx, float dy, float dz, float speed_mph);
BRN_IMPORT("brn.player.v1", "set_infinite_boost") void brn_player_set_infinite_boost(int32_t enabled);
BRN_IMPORT("brn.player.v1", "set_invulnerable")   void brn_player_set_invulnerable(int32_t enabled);
/* The game's debug car change. wheel_id 0 = the car's default wheels. Refused (0) while an
   event is running, like the original debug menu. */
BRN_IMPORT("brn.player.v1", "change_car")    int32_t  brn_player_change_car(uint64_t car_id, uint64_t wheel_id);

/* ---- "brn.vehicles.v1" ------------------------------------------------------------------ */
BRN_IMPORT("brn.vehicles.v1", "count")       int32_t  brn_vehicles_count(void);   /* 0 until the list is loaded */
BRN_IMPORT("brn.vehicles.v1", "get")         int32_t  brn_vehicles_get(int32_t index, BrnVehicleInfo* out);

/* ---- "brn.locations.v1" ----------------------------------------------------------------- */
BRN_IMPORT("brn.locations.v1", "count")      int32_t  brn_locations_count(void);  /* gas stations, junkyards, shops, car parks */
BRN_IMPORT("brn.locations.v1", "get")        int32_t  brn_locations_get(int32_t index, BrnLocation* out);

/* ---- "brn.racecars.v1" ------------------------------------------------------------------ */
BRN_IMPORT("brn.racecars.v1", "count")       int32_t  brn_racecars_count(void);   /* the active race car slots (8) */
BRN_IMPORT("brn.racecars.v1", "get")         int32_t  brn_racecars_get(int32_t index, BrnRaceCarInfo* out);
BRN_IMPORT("brn.racecars.v1", "remove_all_rivals") void brn_racecars_remove_all_rivals(void);

/* ---- "brn.traffic.v1" ------------------------------------------------------------------- */
BRN_IMPORT("brn.traffic.v1", "get")          int32_t  brn_traffic_get(BrnTrafficSettings* out);   /* 0 until traffic exists */
BRN_IMPORT("brn.traffic.v1", "set")          void     brn_traffic_set(const BrnTrafficSettings* in);   /* final_density is ignored */
BRN_IMPORT("brn.traffic.v1", "kill_near_player") void brn_traffic_kill_near_player(void);

/* ---- "brn.debugvars.v1": the game's own debug menu (CgsDev, Ctrl+Space) ------------------
   A component's controls appear only once its section has been opened (call() on it), exactly
   as in the game's menu. */
/* Keeps the listing fresh for about a second; call it every frame while you show it. Returns
   the listing's generation, 0 until the first listing exists. */
BRN_IMPORT("brn.debugvars.v1", "refresh")    uint32_t brn_debugvars_refresh(void);
BRN_IMPORT("brn.debugvars.v1", "count")      int32_t  brn_debugvars_count(void);
BRN_IMPORT("brn.debugvars.v1", "get")        int32_t  brn_debugvars_get(int32_t index, BrnDebugVar* out);
BRN_IMPORT("brn.debugvars.v1", "option_count") int32_t brn_debugvars_option_count(int32_t index);
BRN_IMPORT("brn.debugvars.v1", "get_option") int32_t  brn_debugvars_get_option(int32_t index, int32_t option, char* name, int32_t name_size, int32_t* value);
/* Queued for the next tick. value is text, as the game's console takes it: "true" / "false" for
   a bool (anything but "TRUE" reads as false), "12", "0.5", an option name. */
BRN_IMPORT("brn.debugvars.v1", "set")        void     brn_debugvars_set_n(const char* path, uint32_t path_len, const char* value, uint32_t value_len);
BRN_IMPORT("brn.debugvars.v1", "call")       void     brn_debugvars_call_n(const char* path, uint32_t path_len);

/* ---- "brn.debugini.v1": config.ini [Debug], applied by the game at start-up -------------
   The only part of config.ini a mod can touch. An empty value removes the line. */
BRN_IMPORT("brn.debugini.v1", "set")         int32_t  brn_debugini_set_n(const char* path, uint32_t path_len, const char* value, uint32_t value_len);

/* ---- "brn.storage.v1": the mod's own files, mods/<name>/data/<file> ---------------------
   file: 1..64 characters of [A-Za-z0-9._-], not starting with '.'. No folders. 16 MB a file. */
/* Returns the file's size (copying min(size, capacity) bytes), or -1 when it does not exist. */
BRN_IMPORT("brn.storage.v1", "read")         int32_t  brn_storage_read_n(const char* file, uint32_t file_len, void* out, uint32_t capacity);
BRN_IMPORT("brn.storage.v1", "write")        int32_t  brn_storage_write_n(const char* file, uint32_t file_len, const void* data, uint32_t size);   /* 1 = written */
BRN_IMPORT("brn.storage.v1", "remove")       int32_t  brn_storage_remove_n(const char* file, uint32_t file_len);

/* ---- "brn.ui.v1" ------------------------------------------------------------------------
   Only valid inside brn_mod_render, except the texture calls, which work from anywhere. A
   texture is w*h RGBA8 pixels, rows top to bottom; 0 is never a valid texture. */
BRN_IMPORT("brn.ui.v1", "texture_create")    uint32_t brn_ui_texture_create(const uint8_t* rgba, int32_t w, int32_t h);
BRN_IMPORT("brn.ui.v1", "texture_update")    void     brn_ui_texture_update(uint32_t tex, int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* rgba);
BRN_IMPORT("brn.ui.v1", "texture_destroy")   void     brn_ui_texture_destroy(uint32_t tex);
/* One draw: ni indices (a multiple of 3) into nv vertices, clipped to clip. */
BRN_IMPORT("brn.ui.v1", "submit")            void     brn_ui_submit16(const brn_vertex* v, uint32_t nv, const uint16_t* idx, uint32_t ni,
                                                                     uint32_t tex, float clip_x0, float clip_y0, float clip_x1, float clip_y1, uint32_t flags);
BRN_IMPORT("brn.ui.v1", "submit32")          void     brn_ui_submit32(const brn_vertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni,
                                                                     uint32_t tex, float clip_x0, float clip_y0, float clip_x1, float clip_y1, uint32_t flags);
/* The game hides the Windows cursor; a UI that wants one draws it (ImGui: io.MouseDrawCursor). */
BRN_IMPORT("brn.ui.v1", "set_clipboard_text") void    brn_ui_set_clipboard_text_n(const char* text, uint32_t len);

/* ---- conveniences for NUL-terminated strings ------------------------------------------- */
static inline uint32_t brn__strlen(const char* s) { return s ? (uint32_t)__builtin_strlen(s) : 0u; }
static inline void    brn_log(const char* text) { brn_log_n(text, brn__strlen(text)); }
static inline int32_t brn_has_module(const char* name) { return brn_has_module_n(name, brn__strlen(name)); }
static inline void    brn_debugvars_set(const char* path, const char* value) { brn_debugvars_set_n(path, brn__strlen(path), value, brn__strlen(value)); }
static inline void    brn_debugvars_call(const char* path) { brn_debugvars_call_n(path, brn__strlen(path)); }
static inline int32_t brn_debugini_set(const char* path, const char* value) { return brn_debugini_set_n(path, brn__strlen(path), value, brn__strlen(value)); }
static inline int32_t brn_storage_read(const char* file, void* out, uint32_t capacity) { return brn_storage_read_n(file, brn__strlen(file), out, capacity); }
static inline int32_t brn_storage_write(const char* file, const void* data, uint32_t size) { return brn_storage_write_n(file, brn__strlen(file), data, size); }
static inline int32_t brn_storage_remove(const char* file) { return brn_storage_remove_n(file, brn__strlen(file)); }
static inline void    brn_ui_set_clipboard_text(const char* text) { brn_ui_set_clipboard_text_n(text, brn__strlen(text)); }
static inline void    brn_ui_submit(const brn_vertex* v, uint32_t nv, const uint16_t* idx, uint32_t ni, uint32_t tex, brn_rect clip, uint32_t flags)
{ brn_ui_submit16(v, nv, idx, ni, tex, clip.x0, clip.y0, clip.x1, clip.y1, flags); }

/* ======================================================================================== */
/* The mod: the exports the host calls. Define them with these names (BRN_EXPORT does it in C). */
/* ======================================================================================== */
/*   uint32_t brn_mod_api_version(void);   required, return BRN_MOD_API_VERSION
 *   int32_t  brn_mod_init(void);          required, non-zero = loaded (imports work from here on)
 *   void     brn_mod_tick(void);          update thread, once per game update
 *   void     brn_mod_render(uint32_t width, uint32_t height, float dpi_scale,
 *                           float delta_seconds, uint64_t frame_index);
 *                                         render thread, after the input; submit the UI here.
 *                                         width/height: back buffer pixels; dpi_scale: the
 *                                         window's DPI / 96.
 *   brn_input_event* brn_mod_input_buffer(void);
 *                                         where the host writes up to BRN_INPUT_BUFFER_CAPACITY
 *                                         events (a static array in the mod)
 *   void     brn_mod_on_input(uint32_t count);   that many events are in the buffer
 *   void     brn_mod_shutdown(void);      switched off from the mods list (not after a trap)
 */
#define BRN_DEFINE_API_VERSION() \
    BRN_EXPORT("brn_mod_api_version") uint32_t brn_mod_api_version(void) { return BRN_MOD_API_VERSION; }
#define BRN_DEFINE_INPUT_BUFFER(buffer) \
    BRN_EXPORT("brn_mod_input_buffer") brn_input_event* brn_mod_input_buffer(void) { return (buffer); }

#endif /* __wasm__ */

#ifdef __cplusplus
}
#endif

#endif /* BRN_MOD_API_H */
