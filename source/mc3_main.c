/* mc3_main.c -- Modern Combat 3 on android32: from the player's APK to the
 * game's frame loop.
 *
 * port_load: the runtime has found the APK (by its package) in the game
 *   folder; the setup unpacks lib/armeabi-v7a/libModernCombat3.so next to it
 *   (once; again only when the APK changes), which must be the 1.1.7g build
 *   (its CRC, from the setup's stamp). Then the library is mapped, relocated,
 *   bound to the runtime's bionic shims and sealed.
 *
 * port_run: what the Java activity does on a phone, in the order the PS Vita
 *   port (mc3-vita, main.c) found to work:
 *     JNI_OnLoad; the installer's DRM flags set (three ints the Java installer
 *     would have set, at fixed .bss addresses of this build); the EGL context
 *     (mc3_gl.c); GL2JNILib.init; InAppBilling.nativeInit; MyVideoView.Init;
 *     GloftM3HM.Init; GloftM3HM.nativeMC3Init; GL2JNILib.getViewSettings;
 *     GL2JNILib.resize(window); then one GL2JNILib.step per frame.
 *   The Vita port ran the engine on a thread with a 9 MB stack: here it runs
 *   on the main thread, whose stack the NPDM makes 10 MB (Makefile).
 *
 * Briefing videos (an activity on Android) are played between two frames
 * (mc3_video.c, when the build has FFmpeg), then the game is told the video
 * finished. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_boost.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "jni.h"
#include "port_config.h"
#include "rt_applet.h"
#include "rt_boot.h"
#include "rt_cfg.h"
#include "rt_window.h"
#include "so_util.h"
#include "util.h"

#include "mc3.h"

so_module g_mc3_mod;

/* The installer's "data is valid" flags: pointers in .bss the Java installer
 * points at ints holding 1 (found by the Vita port; DAT_0098a26c/78/7c). */
#define MC3_DRM_LOCK1 0x0098a278u
#define MC3_DRM_LOCK2 0x0098a26cu
#define MC3_DRM_LOCK3 0x0098a27cu
static int g_drm_ok[3] = {1, 1, 1};

typedef int (*jni_onload_fn)(void *vm, void *reserved);
typedef void (*jni_void_fn)(void *env, void *cls);
typedef void (*jni_obj_fn)(void *env, void *cls, void *obj);
typedef void (*jni_size_fn)(void *env, void *cls, int w, int h);
typedef int (*jni_int_ret_fn)(void *env, void *cls);
typedef void (*video_finished_fn)(void *env, void *cls);

static jni_int_ret_fn f_is_gameplay;
static jni_void_fn f_step, f_pause, f_resume;
static video_finished_fn f_video_finished;
static void *g_activity_cls, *g_glf_cls, *g_video_cls;

/* ---------------------------------------------------------------- setup */
static const char *const k_libs[] = {MC3_LIB};

/* After the library: the briefing videos out of the patch OBB (mc3_setup.c);
 * it returns at once when they are there. */
static const RtSetupStep k_steps[] = {
    {"briefing videos", RT_STEP_AFTER_ZIP, 150, 1000, mc3_setup_obb},
};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = 1,
    .apk_requirement = "Modern Combat 3 " MC3_VERSION " (" PORT_PACKAGE ") for armeabi-v7a",
    .libs_p0 = 0,
    .libs_p1 = 150,
    .steps = k_steps,
    .nsteps = 1,
};

const char *port_apk_help(void) {
  return "Copy the APK of your own Modern Combat 3 " MC3_VERSION " to /switch/" PORT_NAME "/\n"
         "(any name), and its two OBB files to /switch/" PORT_NAME "/obb/:\n"
         "  " MC3_OBB_MAIN "\n"
         "  " MC3_OBB_PATCH;
}

int port_load(const char *apk) {
  dcr_setup_from_apk(apk);

  unsigned long crc = 0, size = 0;
  if (rt_setup_stamp_get(MC3_LIB, &crc, &size) && (crc != MC3_LIB_CRC32 || size != MC3_LIB_SIZE))
    fatal_error("This APK's %s is not the one from Modern Combat 3 %s\n"
                "(CRC %08lx, %lu bytes; this port needs %08lx, %lu bytes).\n\n%s",
                MC3_LIB, MC3_VERSION, crc, size, MC3_LIB_CRC32, MC3_LIB_SIZE, port_apk_help());

  char path[DCR_PATH_MAX];
  rt_root_path(path, sizeof path, MC3_LIB);
  if (so_load(&g_mc3_mod, path, NULL, PORT_SO_REGION_BYTES) != 0)
    fatal_error("Could not load %s.", path);
  so_relocate(&g_mc3_mod);
  so_resolve(&g_mc3_mod, NULL, 0, 1);
  so_fix_kuser_helpers(&g_mc3_mod);
  /* ARMv8 has no SWP: the game's spinlocks would die at their first use. */
  if (mc3_swp_fix(&g_mc3_mod) != 0)
    fatal_error("Could not rewrite %s's SWP instructions (see debug.log).", MC3_LIB);
  so_finalize(&g_mc3_mod);
  debugPrintf("[mc3] %s mapped at %p\n", MC3_LIB, g_mc3_mod.load_virtbase);
  return 0;
}

/* ---------------------------------------------------------------- video
 * Asked for from inside a frame (startActivity, mc3_jni.c); played after it
 * (mc3_video.c), then the game is told it finished. */
static volatile int g_video_pending;
static char g_video_path[512];

void mc3_video_requested(const char *name) {
  snprintf(g_video_path, sizeof g_video_path, "%s", name ? name : "");
  g_video_pending = 1;
}

/* ---------------------------------------------------------------- focus */
void port_focus_lost(void) {
  mc3_audio_pause(1);
  if (f_pause)
    f_pause(g_jni_env, g_activity_cls);
}

void port_focus_gained(void) {
  if (f_resume)
    f_resume(g_jni_env, g_activity_cls);
  mc3_audio_pause(0);
}

/* ---------------------------------------------------------------- run */
static void *need(const char *sym) {
  void *p = (void *)so_try_find_addr_rx(&g_mc3_mod, sym);
  if (!p)
    fatal_error("%s has no %s: not the %s build?", MC3_LIB, sym, MC3_VERSION);
  return p;
}

static void set_drm_flags(void) {
  uintptr_t base = (uintptr_t)g_mc3_mod.load_virtbase;
  *(int **)(base + MC3_DRM_LOCK1) = &g_drm_ok[0];
  *(int **)(base + MC3_DRM_LOCK2) = &g_drm_ok[1];
  *(int **)(base + MC3_DRM_LOCK3) = &g_drm_ok[2];
}

#define ACT "Java_com_gameloft_android_ANMP_GloftM3HM_"
#define GLF "Java_com_gameloft_glf_GL2JNILib_"

void port_run(void) {
  jni_init();
  so_execute_init_array(&g_mc3_mod);

  g_activity_cls = jni_class("com/gameloft/android/ANMP/GloftM3HM/GloftM3HM")->obj;
  g_glf_cls = jni_class("com/gameloft/glf/GL2JNILib")->obj;
  g_video_cls = jni_class("video/MyVideoView")->obj;

  jni_onload_fn onload = (jni_onload_fn)need("JNI_OnLoad");
  debugPrintf("[mc3] JNI_OnLoad -> 0x%x\n", onload(g_jni_vm, NULL));
  set_drm_flags();

  if (mc3_gl_init() != 0)
    fatal_error("The graphics could not start (EGL). See debug.log.");

  ((jni_void_fn)need(GLF "init"))(g_jni_env, g_glf_cls);
  debugPrintf("[mc3] GL2JNILib.init done\n");
  ((jni_obj_fn)need(ACT "iab_InAppBilling_nativeInit"))(g_jni_env, g_activity_cls,
                                                         jni_singleton("android/content/Context"));
  ((jni_void_fn)need("Java_video_MyVideoView_Init"))(g_jni_env, g_video_cls);
  ((jni_void_fn)need(ACT "GloftM3HM_Init"))(g_jni_env, g_activity_cls);
  debugPrintf("[mc3] GloftM3HM.Init done\n");
  ((jni_void_fn)need(ACT "GloftM3HM_nativeMC3Init"))(g_jni_env, g_activity_cls);
  debugPrintf("[mc3] nativeMC3Init done\n");
  ((jni_int_ret_fn)need(GLF "getViewSettings"))(g_jni_env, g_glf_cls);

  int w = 1280, h = 720;
  dcr_window_size(&w, &h);
  ((jni_size_fn)need(GLF "resize"))(g_jni_env, g_glf_cls, w, h);
  debugPrintf("[mc3] resize %dx%d\n", w, h);

  f_step = (jni_void_fn)need(GLF "step");
  f_is_gameplay = (jni_int_ret_fn)need(ACT "GloftM3HM_isGamePlay");
  f_pause = (jni_void_fn)so_try_find_addr_rx(&g_mc3_mod, ACT "GloftM3HM_pauseGame");
  f_resume = (jni_void_fn)so_try_find_addr_rx(&g_mc3_mod, ACT "GloftM3HM_resumeGame");
  f_video_finished = (video_finished_fn)so_try_find_addr_rx(&g_mc3_mod, "Java_video_MyVideoView_SetFinished");
  mc3_input_init();

  debugPrintf("[mc3] frame loop\n");
  uint64_t frame = 0;
  while (!rt_exit_requested() && appletMainLoop()) {
    rt_applet_poll();
    if (!rt_focused()) {
      svcSleepThread(50000000LL);
      continue;
    }
    dcr_boost_frame_begin();
    mc3_input_poll(f_is_gameplay(g_jni_env, g_activity_cls) > 0);
    f_step(g_jni_env, g_glf_cls);
    mc3_gl_swap();
    if (g_video_pending) {
      g_video_pending = 0;
      if (g_video_path[0])
        mc3_video_play(g_video_path);
      if (f_video_finished)
        f_video_finished(g_jni_env, g_video_cls);
    }
    dcr_boost_frame_end(++frame);
  }
  rt_applet_stop();
  mc3_audio_shutdown();
  debugPrintf("[mc3] exit after %llu frames\n", (unsigned long long)frame);
  log_flush_ring();
}
