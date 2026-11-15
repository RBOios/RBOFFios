#pragma once
/* Embedding API: run a Ragnarok Battle Offline executable on a background thread.
 * Not re-entrant: the engine keeps global state, so start it once per process. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RboCallbacks {
    void *ctx;
    /* Emulator thread, once per presented frame. pixels = w*h BGRA8, valid only during the call. */
    void (*present)(void *ctx, const uint32_t *pixels, int w, int h);
    /* Emulator thread, once when the game exits (or rbo_stop took effect). */
    void (*finished)(void *ctx, int code);
} RboCallbacks;

/* game_root: folder that contains the game's exe files and DATA/. exe_name e.g. "RBO.exe", "RBO_Ex1.exe". Returns 0 on success. */
int  rbo_start(const char *game_root, const char *exe_name, const RboCallbacks *cb);
void rbo_stop(void);
int  rbo_is_running(void);
/* DirectInput scan code (e.g. 0xC8 up, 0xD0 down, 0xCB left, 0xCD right, 0x1E A, 0x1F S, 0x20 D, 0x21 F). Thread-safe. */
void rbo_set_key(int dik, int down);
/* 1 = pause the emulation (call when the app leaves the foreground), 0 = continue. Safe to call at any time from any thread. */
void rbo_set_paused(int paused);
/* Trainer functions (from the PC trainer rbo_ex3_trainer). Only for RBO_Ex3.exe; for other executables every call does nothing and
 * rbo_cheat_get returns -1. id: 0 HP, 1 SP, 2 skill points, 3 stat points, 4 arrows, 5 money. All writes are carried out by the emulator thread.
 * set = write the value, add = add to the current value, freeze = keep the value it has now (on=1) until freeze(id,0). Safe from any thread. */
int  rbo_cheat_get(int id);
void rbo_cheat_set(int id, int value);
void rbo_cheat_add(int id, int delta);
void rbo_cheat_freeze(int id, int on);
int  rbo_cheat_frozen_mask(void);     /* bit i set = id i is frozen */
void rbo_cheat_reset(void);
/* Performance counters: frames presented so far and total milliseconds spent converting/handing them to the host. */
void rbo_get_stats(int *frames, double *present_ms_total);
/* Pull 44.1 kHz stereo int16 audio (call from the audio thread). Returns frames actually produced; the rest is silence. */
/* Extra counters for diagnosing slowness: total ms spent sleeping (idle) and guest x86 instructions executed so far. */
void rbo_get_stats2(int *frames, double *present_ms_total, double *idle_ms_total, uint64_t *guest_instructions);
/* Profile text: 2 lines of time split (interpreter / HLE / sleeping) followed by the 12 busiest HLE functions over the last interval,
 * one per line. Call once per interval (it keeps its own snapshot). UTF-8, returns length. */
int  rbo_get_prof(char *out, int n);
int  rbo_audio_pull(int16_t *stereo, int frames);
#ifdef __cplusplus
}
#endif
