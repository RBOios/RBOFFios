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

/* game_root: folder that contains the game's exe files and DATA/. exe_name is "RBO_Ex3.exe" (the only supported executable). Returns 0 on success. */
int  rbo_start(const char *game_root, const char *exe_name, const RboCallbacks *cb);
void rbo_stop(void);
int  rbo_is_running(void);
/* DirectInput scan code (e.g. 0xC8 up, 0xD0 down, 0xCB left, 0xCD right, 0x1E A, 0x1F S, 0x20 D, 0x21 F). Thread-safe. */
void rbo_set_key(int dik, int down);
/* 1 = pause the emulation (call when the app leaves the foreground), 0 = continue. Safe to call at any time from any thread. */
void rbo_set_paused(int paused);
/* Output volume 0..1: master = whole mix, bgm = additional factor for the streamed music. */
void rbo_set_volume(float master, float bgm);
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
/* Events (pause on/off, cheat requests, state save/load start+end) since the last call, one per line ("frame=N 일시정지 켬"). Drains the queue. UTF-8, returns length. */
int  rbo_get_events(char *out, int n);
/* Quick save / load (one file per slot). The request is carried out asynchronously by the emulator thread: poll rbo_state_result().
 * op: 1 = save, 2 = load. Returns 0 if accepted, <0 if busy / bad op. */
int  rbo_state_request(int op, const char *path);
/* 0 = no request yet, 1 = working, 2 = saved, 3 = loaded, <0 = failed. rbo_state_message() is a short Korean status text for the last result. */
int  rbo_state_result(void);
const char *rbo_state_message(void);
/* Header of a saved file: unix time and frame counter. Returns 0 if the file exists and is a state file. */
int  rbo_state_info(const char *path, int64_t *unix_time, int *frames);
#ifdef __cplusplus
}
#endif
