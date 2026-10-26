/* mc3_jni.c -- the Java side of Modern Combat 3, answered in C.
 *
 * The game's GLF layer (com/gameloft/glf), its activity (GloftM3HM), the
 * billing, push and video helpers and a few framework classes (AudioTrack,
 * Intent, Bundle) are reached through JNI. The PS Vita port (mc3-vita by
 * Volodymyr Atamanenko, java.c) answered them with FalsoJNI, which matches a
 * method by its NAME alone; the same answers are kept here, through the
 * runtime's port_jni_invoke hook, which sees every call first. Matching by
 * name keeps the Vita port's coverage without knowing which class each
 * method is declared on; the return kind is taken from the signature (m->ret),
 * so a name the game uses on two classes with different return types still
 * answers the right kind.
 *
 * Differences from the Vita port, on purpose:
 *   - folders are Android paths ("/sdcard/Android/obb/<pkg>/..."); the
 *     runtime translates them (dcr_path.c): obb/ and external/ in the game
 *     folder;
 *   - objects are real runtime objects (singletons per class), never the
 *     0x42424242 placeholder;
 *   - trophies and the video player are left out (videos: mc3_main.c skips
 *     them, see mc3_video_*).
 * MIT.
 */
#include <stdio.h>
#include <string.h>

#include "jni.h"
#include "port_config.h"
#include "util.h"

#include "mc3.h"

#define ANDROID_OBB   "/sdcard/Android/obb/" PORT_PACKAGE "/"
#define ANDROID_FILES "/sdcard/Android/data/" PORT_PACKAGE "/files/"
#define APP_FILES     "/data/data/" PORT_PACKAGE "/files/"

/* ------------------------------------------------------------ helpers */
static jvalue ret_str(const char *s) { return jv_l(jni_str(s)); }

/* A placeholder object of the method's own return class, else of its class. */
static jvalue ret_obj_of(const JMethod *m, const char *fallback_cls) {
  const char *r = strchr(m->sig, ')');
  if (r && r[1] == 'L') {
    char cls[112];
    const char *b = r + 2, *e = strchr(b, ';');
    size_t n = e ? (size_t)(e - b) : strlen(b);
    if (n >= sizeof cls)
      n = sizeof cls - 1;
    memcpy(cls, b, n);
    cls[n] = 0;
    if (!strcmp(cls, "java/lang/String"))
      return ret_str("");
    if (!strcmp(cls, "java/lang/Object") && fallback_cls)
      return jv_l(jni_singleton(fallback_cls));
    return jv_l(jni_singleton(cls));
  }
  return jv_l(jni_singleton(fallback_cls ? fallback_cls : m->cls->name));
}

/* ------------------------------------------------------------ strings */
static JNI_H_DECL(h_dummy_str) { return ret_str("dummy"); }
static JNI_H_DECL(h_sd_folder) { return ret_str(ANDROID_FILES); }
static JNI_H_DECL(h_save_folder) { return ret_str(APP_FILES); }
static JNI_H_DECL(h_zip_name) { return ret_str(ANDROID_OBB MC3_OBB_MAIN); }
static JNI_H_DECL(h_patch_file) { return ret_str(ANDROID_OBB MC3_OBB_PATCH); }
static JNI_H_DECL(h_language) { return ret_str("en"); }
static JNI_H_DECL(h_country) { return ret_str("us"); }
static JNI_H_DECL(h_device_name) { return ret_str("Xperia Play"); }
static JNI_H_DECL(h_device_fw) { return ret_str("4.1.2"); }
static JNI_H_DECL(h_imei) { return ret_str("351066496380730"); }
static JNI_H_DECL(h_package) { return ret_str(PORT_PACKAGE); }

/* ------------------------------------------------------------ objects */
static JNI_H_DECL(h_context) { return jv_l(jni_singleton("android/content/Context")); }
static JNI_H_DECL(h_window) { return jv_l(jni_singleton("android/view/Window")); }
static JNI_H_DECL(h_window_manager) { return jv_l(jni_singleton("android/view/WindowManager")); }
static JNI_H_DECL(h_intent) { return jv_l(jni_singleton("android/content/Intent")); }
static JNI_H_DECL(h_object) { return ret_obj_of(m, NULL); }

/* ------------------------------------------------------------ values */
static JNI_H_DECL(h_cores) { return jv_i(3); }
static JNI_H_DECL(h_one) { return jv_i(1); }

/* ------------------------------------------------------------ video
 * The briefings are an activity (video/MyVideoView) started with an Intent
 * whose "video_name" extra names the file. Their playback is not done yet:
 * the name is kept and the main loop reports the video finished right after
 * the frame that started it (mc3_main.c). */
static char g_video_name[256];

static JNI_H_DECL(h_put_extra) {
  const char *k = a ? jni_utf(a[0].l) : "";
  if (!strcmp(k, "video_name"))
    snprintf(g_video_name, sizeof g_video_name, "%s", jni_utf(a[1].l));
  return jv_l(self ? jni_retain(self) : jni_singleton("android/content/Intent"));
}

static JNI_H_DECL(h_start_activity) {
  debugPrintf("[mc3] startActivity: video \"%s\" (skipped)\n", g_video_name);
  mc3_video_requested(g_video_name);
  return jv_none();
}

/* ------------------------------------------------------------ AudioTrack
 * AudioTrack(streamType, sampleRate, channelConfig, audioFormat, bufferSize,
 * mode): the game's mixer writes 16-bit PCM to it from its own thread; the
 * write blocks on the output (mc3_audio.c), which paces that thread as
 * Android's would. */
static JNI_H_DECL(h_audiotrack_init) {
  int rate = a ? a[1].i : 44100;
  int chcfg = a ? a[2].i : 12;
  int fmt = a ? a[3].i : 2;
  if (self) {
    self->v[0] = rate;
    self->v[1] = chcfg == 4 ? 1 : 2; /* CHANNEL_OUT_MONO 4, STEREO 12 */
    self->v[2] = fmt;
  }
  debugPrintf("[mc3] AudioTrack(%d Hz, channels %d, format %d)\n", rate, chcfg, fmt);
  return jv_none();
}

static JNI_H_DECL(h_audiotrack_write) {
  if (!a)
    return jv_i(0);
  JObj *arr = a[0].l;
  jint off = a[1].i, len = a[2].i;
  if (!arr || arr->kind != JK_ARRAY || off < 0 || len <= 0 || off + len > arr->a.len)
    return jv_i(0);
  int rate = self ? (int)self->v[0] : 44100;
  int chans = self && self->v[1] ? (int)self->v[1] : 2;
  if (arr->a.elem == 'S') {
    const int16_t *s = (const int16_t *)arr->a.data + off;
    mc3_audio_write(s, len / chans, chans, rate);
  } else if (arr->a.elem == 'B') {
    const int16_t *s = (const int16_t *)((const uint8_t *)arr->a.data + off);
    mc3_audio_write(s, len / 2 / chans, chans, rate);
  }
  return jv_i(len);
}

static JNI_H_DECL(h_audio_min_buffer) { return jv_i(4096); }
static JNI_H_DECL(h_audio_rate) { return jv_i(44100); }
static JNI_H_DECL(h_audio_play) { mc3_audio_pause(0); return jv_none(); }
static JNI_H_DECL(h_audio_pause) { mc3_audio_pause(1); return jv_none(); }

/* ------------------------------------------------------------ the table
 * name, the class it must be on (NULL: any), the handler. A method not here
 * falls through to the runtime: its default answer, logged once as
 * unhandled (debug.log "[jni] unhandled ..."), which is how new ones show. */
typedef struct {
  const char *name;
  const char *cls;
  JMethodFn fn;
} Mc3Method;

#define AT "android/media/AudioTrack"

static const Mc3Method k_methods[] = {
    /* folders and the expansion files */
    {"getSDFolder", NULL, h_sd_folder},
    {"getSaveFolder", NULL, h_save_folder},
    {"getZipName", NULL, h_zip_name},
    {"getPatchFileLocation", NULL, h_patch_file},
    {"deleteOldData", NULL, jni_h_false},
    {"setupPaths", NULL, jni_h_void},

    /* the device, as the Vita port presented it: an Xperia Play, slid open
     * (the engine's gamepad path: keyboardEvent + the two touch pads) */
    {"isXperiaPlay", NULL, jni_h_true},
    {"isDeviceOpened", NULL, jni_h_true},
    {"mustSwapButtons", NULL, jni_h_false},
    {"isHTCDevice", NULL, jni_h_false},
    {"hasGyroscope", NULL, jni_h_false},
    {"isGyroSensibilityHigh", NULL, jni_h_false},
    {"isGyroSensibilityLow", NULL, jni_h_false},
    {"isGyroPendingToFix", NULL, jni_h_false},
    {"ignoreGyroLowInc", NULL, jni_h_false},
    {"isGyroInvertedX", NULL, jni_h_false},
    {"isInvertedPitchYaw", NULL, jni_h_false},
    {"isSwitchRollToYaw", NULL, jni_h_false},
    {"useGeneration0", NULL, jni_h_false},
    {"useGeneration1", NULL, jni_h_false},
    {"useGeneration2", NULL, jni_h_true},
    {"getProcessorNum", NULL, h_cores},
    {"isFirstRunOfTheDay", NULL, jni_h_false},
    {"isHoneycombDevice", NULL, jni_h_false},
    {"isIceCreamSandwich", NULL, jni_h_false},
    {"isSamsung_I9250", NULL, jni_h_false},
    {"isSamsung_I510", NULL, jni_h_false},
    {"isSamsung_P7100", NULL, jni_h_false},
    {"isAsus_TF201", NULL, jni_h_false},
    {"isHTC_Nexus9", NULL, jni_h_false},
    {"getDeviceLanguage", NULL, h_language},
    {"getCountryCode", NULL, h_country},
    {"getDeviceName", NULL, h_device_name},
    {"getDeviceFW", NULL, h_device_fw},
    {"getIMEI", NULL, h_imei},
    {"getHDIDFVd", NULL, h_imei},
    {"getPackage", NULL, h_package},
    {"IsWifiEnabled", NULL, h_one},

    /* the view, the GL context (made by mc3_gl.c before the game starts) */
    {"createView", NULL, jni_h_void},
    {"setViewSettings", NULL, jni_h_void},
    {"setCurrentContext", NULL, jni_h_false},
    {"enableAccelerometer", NULL, jni_h_void},
    {"enableGyroscope", NULL, jni_h_void},
    {"setCalibratingGyro", NULL, jni_h_void},
    {"resetGyroCalibration", NULL, jni_h_void},
    {"setVisibility", NULL, jni_h_void},
    {"hasWindowFocus", NULL, jni_h_false},
    {"requestWindowFeature", NULL, jni_h_false},
    {"setContentView", NULL, jni_h_void},
    {"setVolumeControlStream", NULL, jni_h_void},
    {"showloading", NULL, jni_h_void},
    {"hideloading", NULL, jni_h_void},
    {"hidelogoscreen", NULL, jni_h_void},
    {"finish", NULL, jni_h_void},
    {"getContext", NULL, h_context},
    {"getWindow", NULL, h_window},
    {"getWindowManager", NULL, h_window_manager},
    {"getIntent", NULL, h_intent},
    {"getSystemService", NULL, h_object},
    {"getText", NULL, h_object},
    {"setText", NULL, jni_h_void},
    {"getResource", NULL, jni_h_null},

    /* online, store, push, browser: nothing to reach (servers are gone) */
    {"getUserAgent", NULL, h_dummy_str},
    {"getPreferenceString", NULL, h_dummy_str},
    {"openBrowser", NULL, jni_h_void},
    {"openGLLive", NULL, jni_h_void},
    {"openIGP", NULL, jni_h_void},
    {"notifyTrophy", NULL, jni_h_void},
    {"sendURLRequest", NULL, jni_h_void},
    {"getServerResponse", NULL, h_dummy_str},
    {"hasServerResponse", NULL, jni_h_false},
    {"showDisclaimer", NULL, jni_h_void},
    {"showCantGoBackPopup", NULL, jni_h_void},
    {"showParentButton", NULL, jni_h_void},
    {"hideParentButton", NULL, jni_h_void},
    {"getTotalItems", NULL, jni_h_zero},
    {"getData", NULL, h_object},
    {"GetState", NULL, jni_h_zero},
    {"HasPushNotification", NULL, jni_h_zero},
    {"LaunchGamebyNotification", NULL, jni_h_zero},
    {"ResetNotificationStatus", NULL, jni_h_void},
    {"ResetLaunchStatus", NULL, jni_h_void},
    {"EnableDisablePushNotifications", NULL, jni_h_void},
    {"ShowAlert", NULL, jni_h_void},
    {"GetTokenID", NULL, h_dummy_str},
    {"GetUserID", NULL, h_dummy_str},
    {"SendPush", NULL, h_dummy_str},
    {"SendPushToMyself", NULL, h_dummy_str},
    {"SetOfflineUserCredential", NULL, jni_h_void},
    {"SetOnlineUserCredential", NULL, jni_h_void},
    {"SetOfflineDeviceCredential", NULL, jni_h_zero},
    {"DeletePush", NULL, jni_h_zero},
    {"GetBundleData", NULL, h_object},
    {"GetJanusToken", NULL, h_dummy_str},

    /* Intent / Bundle (the briefing videos' activity) */
    {"setClassName", NULL, jni_h_self},
    {"putExtra", NULL, h_put_extra},
    {"setFlags", NULL, jni_h_self},
    {"addCategory", NULL, jni_h_self},
    {"getIntExtra", NULL, jni_h_zero},
    {"removeExtra", NULL, jni_h_void},
    {"putString", NULL, jni_h_void},
    {"getString", NULL, h_dummy_str},
    {"getByteArray", NULL, jni_h_null},
    {"putByteArray", NULL, jni_h_void},
    {"getInt", NULL, jni_h_zero},
    {"putInt", NULL, jni_h_void},
    {"getLong", NULL, jni_h_zero},
    {"putLong", NULL, jni_h_void},
    {"containsKey", NULL, jni_h_false},
    {"clear", NULL, jni_h_void},
    {"startActivity", NULL, h_start_activity},
    {"startActivityForResult", NULL, jni_h_void},
    {"setThreadPriority", NULL, jni_h_void},

    /* AudioTrack */
    {"<init>", AT, h_audiotrack_init},
    {"getMinBufferSize", AT, h_audio_min_buffer},
    {"getNativeOutputSampleRate", AT, h_audio_rate},
    {"write", AT, h_audiotrack_write},
    {"play", AT, h_audio_play},
    {"pause", AT, h_audio_pause},
    {"stop", AT, h_audio_pause},
    {"release", AT, jni_h_void},
    {"flush", AT, jni_h_void},
    {"getPlayState", AT, jni_h_zero},
};

#define NMETHODS (sizeof k_methods / sizeof k_methods[0])

static int is_class(const JMethod *m, const JObj *self, const char *cls) {
  if (!strcmp(m->cls->name, cls))
    return 1;
  return self && self->cls && jni_is(self, cls);
}

int port_jni_invoke(JObj *self, JMethod *m, const jvalue *args, jvalue *out) {
  for (unsigned i = 0; i < NMETHODS; i++) {
    const Mc3Method *e = &k_methods[i];
    if (strcmp(e->name, m->name))
      continue;
    if (e->cls && !is_class(m, self, e->cls))
      continue;
    if (g_jni_log)
      debugPrintf("[jni] %s.%s%s (mc3)\n", m->cls->name, m->name, m->sig);
    *out = e->fn(self, args, m);
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------ the tables
 * Everything is answered above; these stay as the runtime expects them. */
const JMethodDef jni_method_defs[] = {{NULL, NULL, NULL, NULL}};

/* Build.VERSION.SDK_INT: the Vita port said 19 (4.4); Context.WINDOW_SERVICE. */
const JFieldDef jni_field_defs[] = {
    {"android/os/Build$VERSION", "SDK_INT", NULL, 19, NULL},
    {"android/content/Context", "WINDOW_SERVICE", NULL, 0, "window"},
    {NULL, NULL, NULL, 0, NULL},
};

const char *const jni_class_supers[][2] = {{NULL, NULL}};
const char *const jni_missing_classes[] = {NULL};
