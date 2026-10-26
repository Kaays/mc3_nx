/* mc3.h -- what the port's files share. MIT. */
#ifndef MC3_H
#define MC3_H
#include <stdint.h>

#include "so_util.h"

extern so_module g_mc3_mod;

/* mc3_setup.c: the setup step that unpacks the briefing videos. */
#include "dcr_setup.h"
void mc3_setup_obb(RtSetupCtx *ctx);
extern int g_mc3_trace_files; /* every opened path to debug.log */

/* mc3_swp.c: SWP -> LDREX/STREX stubs, between so_resolve and so_finalize. */
int mc3_swp_fix(so_module *m);

/* mc3_audio.c: AudioTrack.write -> audout. frames of `chans` (1 or 2)
 * interleaved s16 at `rate` Hz; blocks while the output is full. */
void mc3_audio_write(const int16_t *pcm, int frames, int chans, int rate);
void mc3_audio_pause(int paused);
/* A briefing video takes the output (the game's sound dropped meanwhile). */
void mc3_audio_video_owner(int on);
void mc3_audio_write_video(const int16_t *pcm, int frames, int chans, int rate);
void mc3_audio_shutdown(void);

/* mc3_gl.c: the EGL window context, made before the game's first GL call. */
int mc3_gl_init(void);
void mc3_gl_swap(void);
void mc3_gl_set_interval(int interval);

/* mc3_input.c: controllers and the touch screen -> the engine. */
void mc3_input_init(void);
void mc3_input_poll(int in_gameplay);

/* mc3_main.c: the Flash menu on top ("menu_csbars" in gameplay), or NULL. */
const char *mc3_menu_name(void);

/* mc3_main.c: a briefing video was asked for (mc3_jni.c). */
void mc3_video_requested(const char *name);

/* mc3_video.c: play a video (an Android path) to the end, or until A, B or
 * + skips it; on the thread that owns the GL context, between frames.
 * 0 played, -1 could not (no FFmpeg in this build, unreadable file...). */
int mc3_video_play(const char *android_path);

/* mc3_config.c: config.ini. */
typedef struct {
  int fps60;          /* 1: 60 fps, 0: 30 */
  float look_speed;   /* right stick multiplier */
  int invert_y;       /* right stick Y inverted */
  int touch;          /* touch screen passed to the game */
} Mc3Config;
extern Mc3Config g_mc3_cfg;

#endif /* MC3_H */
