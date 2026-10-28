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
 *   A  the prompt on screen    / select (DPAD_CENTER 23)
 *   L3 sprint (DPAD_UP 19)      D-pad: up sprint, down crouch, left/right weapon
 * The game sprints while DPAD_UP is held. Holding a stick click down while
 * running is hard (hardware: the thumb slipped, sprint/walk flickered, and the
 * breathing sound went on), so by default L3 is a toggle (sprint_toggle): one
 * click and the port holds DPAD_UP until another click, the left stick let go
 * for a moment, aiming (ZL), or leaving play. The D-pad's up stays a hold.
 *   +  START 108 (in a skippable cutscene, menu_SkipAnim: skip it, as the
 *      ">>" button)              -  SELECT 109
 * A in gameplay: what the Xperia Play's centre key does there -- the prompt
 * on screen (a computer, a door, a bomb, a weapon on the ground, a grenade to
 * throw back, a melee counter, a QTE) -- without the key itself, which the
 * Vita port kept off in gameplay for an engine bug and which went wrong here
 * too: mc3_hud_use sets the game's own event for the prompt, only for
 * prompts that are really up (mc3_main.c). On the game's other menus over gameplay (pause,
 * mission failed, debriefing: mc3_menu_name) A selects and B goes back, as
 * in the other menus. The bar filled with a finger (DraggableQTE) also takes
 * the right stick as a finger, for docked play: flick it the way the finger
 * would swipe, again and again.
 *
 * A key held by two buttons at once is pressed once and released when the
 * last one lets go; each button releases the key it pressed even if the
 * game left gameplay meanwhile. MIT.
 */
#include <math.h>
#include <stdio.h>
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
    {HidNpadButton_A, AK_NONE, AK_DPAD_CENTER}, /* in play: mc3_hud_use */
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
    if (g_key_refs[k]++ == 0) {
      debugPrintf("[input] key %d down\n", k);
      f_key(g_jni_env, g_cls, k, 1);
    }
  } else if (g_key_refs[k] > 0) {
    if (--g_key_refs[k] == 0) {
      debugPrintf("[input] key %d up\n", k);
      f_key(g_jni_env, g_cls, k, 0);
    }
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
  if (now != was)
    debugPrintf("[input] stick %d %s\n", pointer, now ? "moving" : "released");
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

/* Back into play: the menus act on key-ups (the log: "processKeyUp = 19" in
 * menu_IGM), so a play key let go while a menu, a cutscene or the pause
 * screen was up is taken by that screen and play never sees it released. On
 * the first frame of play again, sprint (DPAD_UP) is released once more if no
 * button holds it, and both sticks are fed their (0,0) again. Only sprint:
 * the weapon keys act on their key-up, and a stray one switched the weapon at
 * every mission start and every return from the pause menu (hardware). */
static void resync_play(void) {
  if (!f_key)
    return;
  if (!g_key_refs[AK_DPAD_UP])
    f_key(g_jni_env, g_cls, AK_DPAD_UP, 0);
  g_left.last_x = g_left.last_y = g_left.last2_x = g_left.last2_y = 0.0f;
  g_right = g_left;
  g_left.last2_x = g_right.last2_x = 1.0f; /* "was moving": (0,0) next frame */
  debugPrintf("[input] back in play: sprint released\n");
}

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

/* The L3 sprint toggle (see the top of the file). */
static int g_sprint;     /* DPAD_UP held for it */
static int g_sprint_idle; /* frames the left stick has been at rest */

static void sprint_toggle(int game_keys, u64 pressed, u64 held, float lx, float ly) {
  int want = g_sprint;
  if (game_keys && (pressed & HidNpadButton_StickL))
    want = !g_sprint;
  g_sprint_idle = (lx == 0.0f && ly == 0.0f) ? g_sprint_idle + 1 : 0;
  /* a few frames' grace: L3 is often clicked as the stick starts moving */
  if (!game_keys || g_sprint_idle > 8 || (held & HidNpadButton_ZL))
    want = 0;
  if (want != g_sprint) {
    g_sprint = want;
    debugPrintf("[input] sprint %s\n", want ? "on" : "off");
    key(AK_DPAD_UP, want);
  }
}

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

/* The right stick as a finger on the DraggableQTE: down in the middle of the
 * screen when the stick leaves the centre, following it up to a third of
 * the screen away, up when it comes back or the bar goes. */
#define QTE_FINGER 15
static int g_qte_finger;

static void qte_stick(int drag, float x, float y) {
  if (!f_touch)
    return;
  int w = 1280, h = 720;
  dcr_window_size(&w, &h);
  float mag = sqrtf(x * x + y * y);
  int cx = w / 2, cy = h / 2;
  int px = cx + (int)(x * (float)h / 3), py = cy + (int)(y * (float)h / 3);
  if (drag && !g_qte_finger && mag > 0.5f) {
    f_touch(g_jni_env, g_cls, 1, cx, cy, QTE_FINGER);
    f_touch(g_jni_env, g_cls, 2, px, py, QTE_FINGER);
    g_qte_finger = 1;
  } else if (g_qte_finger && drag && mag > 0.3f) {
    f_touch(g_jni_env, g_cls, 2, px, py, QTE_FINGER);
  } else if (g_qte_finger) {
    f_touch(g_jni_env, g_cls, 0, px, py, QTE_FINGER);
    g_qte_finger = 0;
  }
}

void mc3_input_poll(int in_gameplay) {
  padUpdate(&g_pad);
  float sticks[4] = {0};
  u64 held = rt_pad_read(&g_pad, sticks);
  static u64 prev_held;
  u64 pressed = held & ~prev_held;
  prev_held = held;
  /* Which mapping: the game's own top menu decides when it can be read --
   * the HUD (menu_csbars) is play, any other menu (pause, mission failed,
   * debriefing, a skippable animation...) takes A/B. Only when no name can
   * be read does the + tracking stand in. */
  const char *menu = mc3_menu_name();
  static char last_menu[48];
  if (menu && strncmp(menu, last_menu, sizeof last_menu - 1)) {
    snprintf(last_menu, sizeof last_menu, "%s", menu);
    debugPrintf("[input] menu %s\n", last_menu);
  }
  if (menu)
    g_pause_menu = 0;
  else
    track_pause(in_gameplay, pressed, held, sticks[0], sticks[1]);
  int game_keys = in_gameplay && (menu ? !strcmp(menu, "menu_csbars") : !g_pause_menu);
  int prompts = in_gameplay ? mc3_hud_prompts() : 0;
  static int last_prompts;
  if (prompts != last_prompts) {
    debugPrintf("[input] %s\n", prompts & MC3_PROMPT_DRAG  ? "drag bar up: touch and the right stick"
                                 : prompts & MC3_PROMPT_ANY ? "prompt up: A takes it"
                                                            : "no prompt");
    last_prompts = prompts;
  }
  int skip_anim = menu && !strcmp(menu, "menu_SkipAnim");
  if (skip_anim && (pressed & HidNpadButton_Plus))
    mc3_hud_skip();
  static int was_game_keys;
  if (game_keys && !was_game_keys)
    resync_play();
  was_game_keys = game_keys;

  for (unsigned i = 0; i < NMAP; i++) {
    if (k_map[i].button == HidNpadButton_StickL && g_mc3_cfg.sprint_toggle)
      continue; /* sprint_toggle() */
    int down = (held & k_map[i].button) != 0;
    if (down && !g_held_key[i]) {
      int k = game_keys ? k_map[i].key_game : k_map[i].key_menu;
      if (skip_anim && k_map[i].button == HidNpadButton_Plus) {
        g_held_key[i] = -1; /* + skips there (above): no pause, now or when play resumes under it */
        continue;
      }
      if (k) {
        g_held_key[i] = k;
        key(k, 1);
      }
    } else if (!down && g_held_key[i]) {
      key(g_held_key[i], 0);
      g_held_key[i] = 0;
    }
  }

  if (game_keys && (pressed & HidNpadButton_A) && (prompts & MC3_PROMPT_ANY))
    mc3_hud_use();

  /* left: move (y up), the Vita port's 1.1 reach so diagonals hit full speed */
  float lx = sticks[0], ly = sticks[1];
  deadzone(&lx, &ly);
  if (g_mc3_cfg.sprint_toggle || g_sprint)
    sprint_toggle(game_keys && g_mc3_cfg.sprint_toggle, pressed, held, lx, ly);
  feed(&g_left, f_pad_left, lx * 1.10f, ly * 1.10f, 1);

  /* right: look (the engine's y grows downward) */
  float rx = sticks[2], ry = sticks[3];
  deadzone(&rx, &ry);
  qte_stick(prompts & MC3_PROMPT_DRAG, rx, -ry); /* screen y grows downwards */
  float sp = g_mc3_cfg.look_speed;
  feed(&g_right, f_pad_right, rx * sp, (g_mc3_cfg.invert_y ? ry : -ry) * sp, 2);

  poll_touch();
}
