/* mc3_input.c -- controllers and the touch screen, into the engine.
 *
 * The game's own controller support is the Xperia Play's (the Vita port
 * presents the device as one, mc3_jni.c): buttons arrive as Android key
 * codes through GL2JNILib.keyboardEvent(key, down), and the two sticks as
 * the Xperia's touch pads, GL2JNILib.nativeSetTouchPadDTLeft (move, pointer
 * 1) and nativeSetTouchPadDT (look, pointer 2), each -1..1. The stick feed
 * follows the Vita port's: a value every frame while held, and (0,0) on the
 * first two frames after release (the engine stops on that).
 *
 * Buttons (in gameplay / in menus):
 *   ZR fire (R1 103)            ZL aim (L1 102)
 *   Y  reload (X 99)            X  BUTTON_Y 100
 *   R  grenade (MOVE_END 123)   L  next weapon (DPAD_RIGHT 22)
 *   B  crouch (DPAD_DOWN 20)  / back (BACK 4)
 *   A  --                     / select (DPAD_CENTER 23)
 *   L3 sprint (DPAD_UP 19)      D-pad: up sprint, down crouch, left/right weapon
 *   +  START 108                -  SELECT 109
 * A is off in gameplay as on the Vita: DPAD_CENTER there triggers an engine
 * bug the Vita port found. In the pause menu (opened with +, see
 * track_pause) A selects and B goes back, as in the other menus. In-game
 * prompts (doors, pick-ups) are the game's on-screen buttons: the touch
 * screen in handheld.
 *
 * A key held by two buttons at once is pressed once and released when the
 * last one lets go; each button releases the key it pressed even if the
 * game left gameplay meanwhile. MIT.
 */
#include <math.h>
#include <string.h>
#include <switch.h>

#include "jni.h"
#include "rt_pad.h"
#include "rt_window.h"
#include "so_util.h"
#include "util.h"

#include "mc3.h"

enum {
  AK_NONE = 0,
  AK_BACK = 4,
  AK_DPAD_UP = 19,
  AK_DPAD_DOWN = 20,
  AK_DPAD_LEFT = 21,
  AK_DPAD_RIGHT = 22,
  AK_DPAD_CENTER = 23,
  AK_BUTTON_X = 99,
  AK_BUTTON_Y = 100,
  AK_BUTTON_L1 = 102,
  AK_BUTTON_R1 = 103,
  AK_BUTTON_START = 108,
  AK_BUTTON_SELECT = 109,
  AK_MOVE_END = 123,
  AK_MAX = 128
};

typedef struct {
  u64 button;
  int key_game, key_menu; /* gameplay / menus */
} Map;

static const Map k_map[] = {
    {HidNpadButton_ZR, AK_BUTTON_R1, AK_BUTTON_R1},
    {HidNpadButton_ZL, AK_BUTTON_L1, AK_BUTTON_L1},
    {HidNpadButton_Y, AK_BUTTON_X, AK_BUTTON_X},
    {HidNpadButton_X, AK_BUTTON_Y, AK_BUTTON_Y},
    {HidNpadButton_R, AK_MOVE_END, AK_MOVE_END},
    {HidNpadButton_L, AK_DPAD_RIGHT, AK_NONE},
    {HidNpadButton_B, AK_DPAD_DOWN, AK_BACK},
    {HidNpadButton_A, AK_NONE, AK_DPAD_CENTER},
    {HidNpadButton_StickL, AK_DPAD_UP, AK_NONE},
    {HidNpadButton_Up, AK_DPAD_UP, AK_DPAD_UP},
    {HidNpadButton_Down, AK_DPAD_DOWN, AK_DPAD_DOWN},
    {HidNpadButton_Left, AK_DPAD_LEFT, AK_DPAD_LEFT},
    {HidNpadButton_Right, AK_DPAD_RIGHT, AK_DPAD_RIGHT},
    {HidNpadButton_Plus, AK_BUTTON_START, AK_BUTTON_START},
    {HidNpadButton_Minus, AK_BUTTON_SELECT, AK_BUTTON_SELECT},
};
#define NMAP (sizeof k_map / sizeof k_map[0])

typedef void (*touch_fn)(void *env, void *cls, int action, int x, int y, int id);
typedef void (*key_fn)(void *env, void *cls, int key, int down);
typedef void (*pad_fn)(void *env, void *cls, float x, float y, int pointer);

static touch_fn f_touch;
static key_fn f_key;
static pad_fn f_pad_left, f_pad_right;
static void *g_cls;

static PadState g_pad;
static int g_held_key[NMAP];   /* the key each button pressed, 0 = none */
static int g_key_refs[AK_MAX]; /* how many buttons hold each key */

#define DEADZONE 0.2f

void mc3_input_init(void) {
  f_touch = (touch_fn)so_try_find_addr_rx(&g_mc3_mod, "Java_com_gameloft_glf_GL2JNILib_touchEvent");
  f_key = (key_fn)so_try_find_addr_rx(&g_mc3_mod, "Java_com_gameloft_glf_GL2JNILib_keyboardEvent");
  f_pad_left = (pad_fn)so_try_find_addr_rx(&g_mc3_mod, "Java_com_gameloft_glf_GL2JNILib_nativeSetTouchPadDTLeft");
  f_pad_right = (pad_fn)so_try_find_addr_rx(&g_mc3_mod, "Java_com_gameloft_glf_GL2JNILib_nativeSetTouchPadDT");
  g_cls = jni_class("com/gameloft/glf/GL2JNILib")->obj;
  rt_pad_setup(1, 1);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  debugPrintf("[input] touch %s, keys %s, pads %s/%s\n", f_touch ? "ok" : "MISSING", f_key ? "ok" : "MISSING",
              f_pad_left ? "ok" : "MISSING", f_pad_right ? "ok" : "MISSING");
}

static void key(int k, int down) {
  if (k <= 0 || k >= AK_MAX || !f_key)
    return;
  if (down) {
    if (g_key_refs[k]++ == 0)
      f_key(g_jni_env, g_cls, k, 1);
  } else if (g_key_refs[k] > 0) {
    if (--g_key_refs[k] == 0)
      f_key(g_jni_env, g_cls, k, 0);
  }
}

/* The Vita port's radial deadzone: below it 0, above it rescaled to 0..1. */
static void deadzone(float *x, float *y) {
  float mag = sqrtf(*x * *x + *y * *y);
  if (mag < DEADZONE) {
    *x = *y = 0.0f;
    return;
  }
  float k = (mag > 1.0f ? 1.0f : (mag - DEADZONE) / (1.0f - DEADZONE)) / mag;
  *x *= k;
  *y *= k;
}

/* One stick: a value while it is off centre, then (0,0) twice. */
typedef struct {
  float last_x, last_y, last2_x, last2_y;
} Stick;

static void feed(Stick *s, pad_fn fn, float x, float y, int pointer) {
  if (!fn)
    return;
  int now = x != 0.0f || y != 0.0f;
  int was = s->last_x != 0.0f || s->last_y != 0.0f;
  int was2 = s->last2_x != 0.0f || s->last2_y != 0.0f;
  if (now || was)
    fn(g_jni_env, g_cls, x, y, pointer);
  else if (was2)
    fn(g_jni_env, g_cls, 0.0f, 0.0f, pointer);
  s->last2_x = s->last_x;
  s->last2_y = s->last_y;
  s->last_x = x;
  s->last_y = y;
}

static Stick g_left, g_right;

static void poll_touch(void) {
  static HidTouchScreenState old;
  HidTouchScreenState st;
  if (!f_touch || !hidGetTouchScreenStates(&st, 1))
    return;
  if (!g_mc3_cfg.touch)
    st.count = 0;
  int w = 1280, h = 720;
  dcr_window_size(&w, &h);
  /* the panel is 1280x720 in either mode; the game's surface is the window */
  for (int i = 0; i < st.count; i++) {
    int was = 0;
    for (int j = 0; j < old.count; j++)
      if (old.touches[j].finger_id == st.touches[i].finger_id)
        was = 1;
    int x = (int)st.touches[i].x * w / 1280, y = (int)st.touches[i].y * h / 720;
    /* GLF: 1 down, 2 move, 0 up (as the Vita port feeds it) */
    f_touch(g_jni_env, g_cls, was ? 2 : 1, x, y, (int)st.touches[i].finger_id);
  }
  for (int j = 0; j < old.count; j++) {
    int still = 0;
    for (int i = 0; i < st.count; i++)
      if (old.touches[j].finger_id == st.touches[i].finger_id)
        still = 1;
    if (!still) {
      int x = (int)old.touches[j].x * w / 1280, y = (int)old.touches[j].y * h / 720;
      f_touch(g_jni_env, g_cls, 0, x, y, (int)old.touches[j].finger_id);
    }
  }
  old = st;
}

/* The pause menu: the engine still reports gameplay (isGamePlay > 0) while it
 * is up, so the menu mapping (A select, B back) would never apply there and A
 * did nothing (hardware: "Main Menu" in the pause menu could not be chosen).
 * No engine flag tells it apart, so the port follows it: + in gameplay opens
 * it (and sends START, which is what opens it in the game); it is taken as
 * closed on + again, when the game leaves gameplay (main menu, loading), or
 * as soon as the player plays again (the left stick pushed, ZR or ZL). */
static int g_pause_menu;

static void track_pause(int in_gameplay, u64 pressed, u64 held, float lx, float ly) {
  if (!in_gameplay) {
    g_pause_menu = 0;
    return;
  }
  if (pressed & HidNpadButton_Plus) {
    g_pause_menu = !g_pause_menu;
    debugPrintf("[input] pause menu %s\n", g_pause_menu ? "open" : "closed (+)");
    return;
  }
  if (g_pause_menu && ((held & (HidNpadButton_ZR | HidNpadButton_ZL)) || lx * lx + ly * ly > 0.36f)) {
    g_pause_menu = 0;
    debugPrintf("[input] pause menu closed (playing again)\n");
  }
}

void mc3_input_poll(int in_gameplay) {
  padUpdate(&g_pad);
  float sticks[4] = {0};
  u64 held = rt_pad_read(&g_pad, sticks);
  static u64 prev_held;
  u64 pressed = held & ~prev_held;
  prev_held = held;
  static int was_gameplay;
  if (!in_gameplay && was_gameplay && g_pause_menu)
    debugPrintf("[input] pause menu closed (left gameplay)\n");
  was_gameplay = in_gameplay;
  track_pause(in_gameplay, pressed, held, sticks[0], sticks[1]);
  int game_keys = in_gameplay && !g_pause_menu;

  for (unsigned i = 0; i < NMAP; i++) {
    int down = (held & k_map[i].button) != 0;
    if (down && !g_held_key[i]) {
      int k = game_keys ? k_map[i].key_game : k_map[i].key_menu;
      if (k) {
        g_held_key[i] = k;
        key(k, 1);
      }
    } else if (!down && g_held_key[i]) {
      key(g_held_key[i], 0);
      g_held_key[i] = 0;
    }
  }

  /* left: move (y up), the Vita port's 1.1 reach so diagonals hit full speed */
  float lx = sticks[0], ly = sticks[1];
  deadzone(&lx, &ly);
  feed(&g_left, f_pad_left, lx * 1.10f, ly * 1.10f, 1);

  /* right: look (the engine's y grows downward) */
  float rx = sticks[2], ry = sticks[3];
  deadzone(&rx, &ry);
  float sp = g_mc3_cfg.look_speed;
  feed(&g_right, f_pad_right, rx * sp, (g_mc3_cfg.invert_y ? ry : -ry) * sp, 2);

  poll_touch();
}
