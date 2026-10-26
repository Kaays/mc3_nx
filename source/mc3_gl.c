/* mc3_gl.c -- the GL context the game's Java side would have made.
 *
 * On Android GL2JNIActivity's GLSurfaceView makes an EGL window surface and
 * a GLES 2 context, then calls into the engine from its render thread; the
 * engine itself never calls EGL (libModernCombat3.so does not link libEGL).
 * So the port makes them once, on the thread that then runs the engine,
 * before the engine's first GL call (GL2JNILib_init loads shaders). MIT.
 */
#define EGL_NO_X11
#include <EGL/egl.h>
#include <switch.h>

#include "dcr_boost.h"
#include "rt_window.h"
#include "util.h"

#include "mc3.h"

void log_console_close(void);

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

int mc3_gl_init(void) {
  log_console_close(); /* the console's framebuffer must let go of the window */
  dcr_window_prepare();

  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY || !eglInitialize(s_dpy, NULL, NULL)) {
    debugPrintf("[egl] no display (0x%x)\n", eglGetError());
    return -1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);

  /* GLSurfaceView's default for GLES 2 is RGB565 + depth 16; the engine was
   * tuned on devices that gave it RGBA8888 + depth 24 (the Xperia Play). */
  static const EGLint cfg_attrs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(s_dpy, cfg_attrs, &cfg, 1, &n) || n < 1) {
    debugPrintf("[egl] no RGBA8 D24S8 config (0x%x)\n", eglGetError());
    return -1;
  }
  s_surf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)nwindowGetDefault(), NULL);
  if (s_surf == EGL_NO_SURFACE) {
    debugPrintf("[egl] no window surface (0x%x)\n", eglGetError());
    return -1;
  }
  static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
  if (s_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
    debugPrintf("[egl] no GLES 2 context (0x%x)\n", eglGetError());
    return -1;
  }
  mc3_gl_set_interval(g_mc3_cfg.fps60 ? 1 : 2);

  int w = 0, h = 0;
  dcr_window_size(&w, &h);
  debugPrintf("[egl] GLES 2 context, %dx%d, %d fps\n", w, h, g_mc3_cfg.fps60 ? 60 : 30);
  return 0;
}

void mc3_gl_set_interval(int interval) {
  if (s_dpy != EGL_NO_DISPLAY)
    eglSwapInterval(s_dpy, interval);
}

void mc3_gl_swap(void) {
  static int first = 1;
  if (first) {
    first = 0;
    dcr_boost_first_picture();
  }
  eglSwapBuffers(s_dpy, s_surf);
  dcr_boost_launch_tick();
}
