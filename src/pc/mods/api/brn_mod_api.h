/*
 * brn_mod_api.h -- the C interface between Burnout_PC.exe and the DLL mods in mods/<name>/.
 *
 * FLAG PC divergence: there is no console counterpart. This header is the only file a mod
 * shares with the game. Plain C, no game type and no third-party library, so a mod can be
 * written in C, C++ or Rust (bindgen or a hand-written mirror).
 *
 * A mod is a folder mods/<name>/ holding mod.ini:
 *     [Mod]
 *     Name=Debug menu
 *     Dll=debug_menu.dll
 *     Version=0.1
 * and that DLL, which exports:
 *     uint32_t brn_mod_api_version(void);                 returns BRN_MOD_API_VERSION
 *     int      brn_mod_init(const BrnHostApi*, BrnModCallbacks*);   non-zero = loaded
 *
 * Everything the game offers is a SERVICE, fetched by name with BrnHostApi::get_service:
 * "brn.core.v1", "brn.player.v1", ... A service table never changes once published; a
 * different shape gets a new name ("brn.player.v2") and the old one stays.
 *
 * Threads:
 *   on_message, on_frame  render thread (messages are queued from the window thread and
 *                         replayed in order just before on_frame)
 *   on_game_tick          update thread, once per game update, after game-state pre-world
 *   service getters       any thread; they return a copy taken at the last game tick
 *   service commands      any thread; queued, applied at the next game tick
 * A mod therefore never touches game memory and never needs its own locking for it.
 */
#ifndef BRN_MOD_API_H
#define BRN_MOD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRN_MOD_API_VERSION 1u

#if defined(_WIN32)
#  define BRN_MOD_EXPORT __declspec(dllexport)
#else
#  define BRN_MOD_EXPORT
#endif

typedef struct BrnMod BrnMod;   /* opaque: this mod, as the host sees it */

typedef struct BrnVec3 { float x, y, z; } BrnVec3;

/* ---- what the mod gives the host ---------------------------------------------------- */

typedef struct BrnFrameContext
{
    void*    d3d9_device;     /* IDirect3DDevice9*, inside the scene, back buffer bound */
    void*    hwnd;            /* the game window (HWND) */
    uint32_t width, height;   /* back buffer size in pixels */
    float    delta_seconds;   /* wall time since the previous frame */
    uint64_t frame_index;
} BrnFrameContext;

#define BRN_MODIFIER_CTRL  1u
#define BRN_MODIFIER_SHIFT 2u
#define BRN_MODIFIER_ALT   4u
#define BRN_MODIFIER_SUPER 8u

typedef struct BrnWindowMessage
{
    void*    hwnd;
    uint32_t msg;             /* WM_* */
    uint64_t wparam;
    int64_t  lparam;
    uint32_t modifiers;       /* BRN_MODIFIER_* held when the window thread received it */
} BrnWindowMessage;

typedef struct BrnModCallbacks
{
    uint32_t size;            /* host fills sizeof(BrnModCallbacks) before brn_mod_init */
    void*    user;            /* passed back to every callback */
    void (*on_message)(void* user, const BrnWindowMessage* message);
    void (*on_frame)(void* user, const BrnFrameContext* frame);
    void (*on_game_tick)(void* user);
    void (*on_shutdown)(void* user);
} BrnModCallbacks;

/* Valid only during brn_mod_init: keep `self` and the service tables, not this struct.
   on_shutdown is reserved; this host does not call it yet (the process just exits). */
typedef struct BrnHostApi
{
    uint32_t    size;
    uint32_t    api_version;
    BrnMod*     self;
    const void* (*get_service)(const char* name);   /* NULL when this build lacks it */
} BrnHostApi;

/* ---- "brn.core.v1" ---------------------------------------------------------------------- */

#define BRN_CAPTURE_KEYBOARD 1u   /* the game stops reading the keyboard */
#define BRN_CAPTURE_MOUSE    2u
#define BRN_CAPTURE_GAMEPAD  4u   /* the game stops reading the pads too */

typedef struct BrnModInfo
{
    char name[64];
    char folder[64];
    char version[32];
    int  loaded;              /* the DLL is in memory */
    int  enabled;             /* its callbacks run */
    char error[128];          /* why it is not loaded, when it is not */
} BrnModInfo;

typedef struct BrnCoreV1
{
    uint32_t size;
    void        (*log)(BrnMod* self, const char* text);            /* to BrnGame.log */
    const char* (*mod_directory)(BrnMod* self);                    /* "...\mods\<name>\" */
    void        (*set_input_capture)(BrnMod* self, uint32_t flags);/* BRN_CAPTURE_* */
    int         (*mod_count)(void);
    int         (*get_mod_info)(int index, BrnModInfo* out);
    void        (*set_mod_enabled)(int index, int enabled);        /* saved in mods/mods.ini */
} BrnCoreV1;

/* ---- "brn.player.v1" -------------------------------------------------------------------- */

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

typedef struct BrnPlayerV1
{
    uint32_t size;
    int  (*get_state)(BrnPlayerState* out);                  /* returns valid */
    /* Same car, new pose (the game's debug teleport). A zero direction keeps the heading. */
    void (*teleport)(BrnVec3 position, BrnVec3 direction);
    /* The game's "put me back on the road" reset, driving at speed_mph. */
    void (*reset_on_track)(float speed_mph);
    /* Drop the car at a pose already moving forward at speed_mph (place-on-track). */
    void (*place_on_track)(BrnVec3 position, BrnVec3 direction, float speed_mph);
    void (*set_infinite_boost)(int enabled);                 /* held until turned off */
    void (*set_invulnerable)(int enabled);                   /* held until turned off */
    /* The game's debug car change. wheel_id 0 = the car's default wheels. Refused (0)
       while an event is running, like the original debug menu. */
    int  (*change_car)(uint64_t car_id, uint64_t wheel_id);
} BrnPlayerV1;

/* ---- "brn.vehicles.v1" ------------------------------------------------------------------ */

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

typedef struct BrnVehiclesV1
{
    uint32_t size;
    int (*count)(void);           /* 0 until the vehicle list is loaded */
    int (*get)(int index, BrnVehicleInfo* out);
} BrnVehiclesV1;

/* ---- "brn.locations.v1" ----------------------------------------------------------------- */

typedef struct BrnLocation
{
    char    label[96];            /* "Gas Station in Downtown, Paradise City" */
    int     type;                 /* BrnTrigger::GenericRegion::Type */
    char    type_name[32];
    BrnVec3 position;             /* in front of the region, as the debug menu spawns */
    BrnVec3 direction;
} BrnLocation;

typedef struct BrnLocationsV1
{
    uint32_t size;
    int (*count)(void);           /* gas stations, junkyards, shops, car parks */
    int (*get)(int index, BrnLocation* out);
} BrnLocationsV1;

/* ---- "brn.racecars.v1" ------------------------------------------------------------------ */

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

typedef struct BrnRaceCarsV1
{
    uint32_t size;
    int  (*count)(void);          /* the active race car slots (8) */
    int  (*get)(int index, BrnRaceCarInfo* out);
    void (*remove_all_rivals)(void);
} BrnRaceCarsV1;

/* ---- "brn.traffic.v1" ------------------------------------------------------------------- */

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

typedef struct BrnTrafficV1
{
    uint32_t size;
    int  (*get)(BrnTrafficSettings* out);      /* 0 until traffic exists */
    void (*set)(const BrnTrafficSettings* in);  /* final_density is ignored */
    void (*kill_near_player)(void);
} BrnTrafficV1;

/* ---- "brn.debugvars.v1" ---------------------------------------------------------------- */
/* The game's own debug menu (CgsDev, Ctrl+Space): its tree of folders, variables and actions.
   A component's controls appear only once its section has been opened (call() on it), exactly
   as in the game's menu. */

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

typedef struct BrnDebugVarsV1
{
    uint32_t size;
    /* Keeps the listing fresh for about a second; call it every frame while you show it.
       Returns the listing's generation, 0 until the first listing exists. */
    uint32_t (*refresh)(void);
    int  (*count)(void);
    int  (*get)(int index, BrnDebugVar* out);
    int  (*option_count)(int index);
    int  (*get_option)(int index, int option, char* name, int name_size, int* value);
    /* Queued for the next tick. value is text, as the game's console takes it: "true" / "false"
       for a bool (anything but "TRUE" reads as false), "12", "0.5", an option name. The game
       clamps it and fires the variable's change callback. */
    void (*set)(const char* path, const char* value);
    void (*call)(const char* path);
} BrnDebugVarsV1;

#ifdef __cplusplus
}
#endif

#endif /* BRN_MOD_API_H */
