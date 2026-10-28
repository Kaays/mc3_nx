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

/* ---------------------------------------------------------------- menus
 * The name of the Flash menu on top (the game prints it on each key as
 * "menuName = %s", libModernCombat3.so+0x162d34). Read the way that code
 * reads it -- the menu manager in .bss, its map lookup (key -1: the current
 * player), the top of that player's menu stack, an STLport string -- with the
 * engine's own two getters, both plain reads. In gameplay it is
 * "menu_csbars" (the HUD); the pause menu, mission failed and debriefing
 * screens are others ("menu_IGM", "menu_fail", "menu_DEBRIEFING"), while the
 * engine still reports gameplay. NULL when there is none (yet). */
#define MC3_MENU_MGR   0x009508e4u /* .bss: the Flash menu manager */
#define MC3_MENU_FIND  0x001d7878u /* map lookup: (manager, -1) -> node */
#define MC3_MENU_TOP   0x0047c550u /* (player) -> top of its menu stack */

const char *mc3_menu_name(void) {
  uintptr_t b = (uintptr_t)g_mc3_mod.load_virtbase;
  if (!b)
    return NULL;
  void *mgr = *(void **)(b + MC3_MENU_MGR);
  if (!mgr)
    return NULL;
  uint8_t *(*find)(void *, int) = (uint8_t * (*)(void *, int))(b + MC3_MENU_FIND);
  uint8_t *(*top)(void *) = (uint8_t * (*)(void *))(b + MC3_MENU_TOP);
  uint8_t *node = find(mgr, -1);
  if (!node)
    return NULL;
  void *player = *(void **)(node + 0xc);
  if (!player)
    return NULL;
  uint8_t *m = top(player);
  if (!m)
    return NULL;
  return (int8_t)m[8] != -1 ? (const char *)(m + 9) : *(const char **)(m + 0x14);
}

/* ---------------------------------------------------------------- hud
 * What the Xperia Play's centre key would act on. The game's own key path
 * (libModernCombat3.so+0x20bae0, the Hud's controls: Xperia keys through the
 * engine's key state) turns DPAD_CENTER into a Hud event when one of these
 * HUD controls is up, and does nothing with it otherwise:
 *   ClickableObject (+0x7ac, the button that follows a computer, a door...),
 *   ClickableQTE (+0x7b0), DraggableQTE (+0x7b4)            -> event 34, "use"
 *   GrenadeThrowback (+0x7f0) -> 23   MeleeBack (+0x7f4) -> 10
 *   +0xa78 -> 4, +0xa7c -> 5, +0x818 -> 37 (the other on-screen prompts)
 * A control is up when its byte +0x78 is set. The Hud is the game object's
 * (.bss +0x950e08) +0x18, the one that function is called with (+0x14d1c4).
 * Plain reads, each pointer checked. Bit 1: a prompt is up; bit 2: the
 * DraggableQTE (the bar filled with a finger). */
#define MC3_GAME_OBJ 0x00950e08u

static int ctl_up(const uint8_t *hud, unsigned off) {
  const uint8_t *c = *(const uint8_t *const *)(hud + off);
  return c && c[0x78];
}

int mc3_hud_prompts(void) {
  uintptr_t b = (uintptr_t)g_mc3_mod.load_virtbase;
  if (!b)
    return 0;
  const uint8_t *gp = *(const uint8_t *const *)(b + MC3_GAME_OBJ);
  if (!gp)
    return 0;
  const uint8_t *hud = *(const uint8_t *const *)(gp + 0x18);
  if (!hud)
    return 0;
  static const unsigned prompts[] = {0x7ac, 0x7b0, 0x7b4, 0x7f0, 0x7f4, 0xa78, 0xa7c, 0x818};
  int r = 0;
  for (unsigned i = 0; i < sizeof prompts / sizeof prompts[0]; i++)
    if (ctl_up(hud, prompts[i]))
      r |= MC3_PROMPT_ANY;
  if (ctl_up(hud, 0x7b4))
    r |= MC3_PROMPT_DRAG;
  return r;
}

/* ---------------------------------------------------------------- logo
 * On Android the activity showed Gameloft's logo video before the game
 * (the engine knows it as <SD folder>/video/LOGO_Gameloft_320x480.mp4).
 * Played from the first place it is found; nothing when it is not. */
static void play_logo(void) {
  static const char *const where[] = {
      "/sdcard/Android/data/" PORT_PACKAGE "/files/video/LOGO_Gameloft_320x480.mp4",
      "/sdcard/Android/data/" PORT_PACKAGE "/files/data/briefing/video/LOGO_Gameloft_320x480.mp4",
      "/sdcard/Android/data/" PORT_PACKAGE "/files/data/briefing/LOGO_Gameloft_320x480.mp4",
  };
  for (unsigned i = 0; i < sizeof where / sizeof where[0]; i++) {
    char buf[DCR_PATH_MAX];
    if (rt_file_size(dcr_translate_path(where[i], buf, sizeof buf)) > 0) {
      mc3_video_play(where[i]);
      return;
    }
  }
  debugPrintf("[video] no logo video found (LOGO_Gameloft_320x480.mp4)\n");
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
  play_logo();

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
