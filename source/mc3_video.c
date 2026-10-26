/* mc3_video.c -- the briefing videos (an activity, video/MyVideoView, on
 * Android), played in the game's own window.
 *
 * When the game starts the activity (mc3_jni.c: startActivity, the path in
 * the Intent's "video_name"), the frame loop (mc3_main.c) calls
 * mc3_video_play between two engine frames, then tells the game the video
 * finished (MyVideoView.SetFinished), as the activity did when it closed.
 *
 * Decoding: a small FFmpeg (tools/build_ffmpeg.sh: the MP4 demuxer, H.264,
 * MPEG-4 and AAC), single-threaded. Picture: the decoder's YUV 4:2:0 planes
 * go to three luminance textures, drawn letterboxed by a BT.601 shader.
 * Sound: the AAC frames, as s16 stereo, take the game's output meanwhile
 * (mc3_audio.c). Pacing: the clock starts at the first picture; each picture
 * waits for its time; the sound's blocking writes keep it at most a few
 * buffers ahead. A, B or + (or a tap) skips. The game's GL state is saved
 * before and put back after: the engine keeps its own copy of it.
 *
 * Without FFmpeg (MC3_VIDEO unset: no portlibs32/lib/libavcodec.a) it only
 * says so; the game then goes on as if the video had played. MIT.
 */
#include <stdint.h>
#include <string.h>

#include "util.h"

#include "mc3.h"

#if !MC3_VIDEO

int mc3_video_play(const char *android_path) {
  debugPrintf("[video] %s: skipped (built without FFmpeg: tools/build_ffmpeg.sh)\n", android_path);
  return -1;
}

#else /* MC3_VIDEO */

#include <stdlib.h>
#include <switch.h>
#include <GLES2/gl2.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>

#include "dcr_path.h"
#include "rt_applet.h"
#include "rt_window.h"

/* ---------------------------------------------------------------- GL */
static const char *k_vs =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

/* BT.601, limited range (what H.264 encoders of the time wrote). */
static const char *k_fs =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D t_y, t_u, t_v;\n"
    "void main() {\n"
    "  float y = 1.1643 * (texture2D(t_y, v_uv).r - 0.0625);\n"
    "  float u = texture2D(t_u, v_uv).r - 0.5;\n"
    "  float v = texture2D(t_v, v_uv).r - 0.5;\n"
    "  gl_FragColor = vec4(y + 1.5958 * v, y - 0.39173 * u - 0.81290 * v, y + 2.017 * u, 1.0);\n"
    "}\n";

static GLuint g_prog, g_tex[3];
static GLint g_a_pos, g_a_uv;

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[256];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    debugPrintf("[video] shader: %s\n", log);
  }
  return s;
}

static int gl_setup(void) {
  if (g_prog)
    return 0;
  GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = compile(GL_FRAGMENT_SHADER, k_fs);
  g_prog = glCreateProgram();
  glAttachShader(g_prog, vs);
  glAttachShader(g_prog, fs);
  glLinkProgram(g_prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    debugPrintf("[video] program does not link\n");
    glDeleteProgram(g_prog);
    g_prog = 0;
    return -1;
  }
  g_a_pos = glGetAttribLocation(g_prog, "a_pos");
  g_a_uv = glGetAttribLocation(g_prog, "a_uv");
  glGenTextures(3, g_tex);
  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, g_tex[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  return 0;
}

/* What the engine may have cached and the player changes. */
typedef struct {
  GLint prog, active_tex, tex[3], array_buf, elem_buf, viewport[4], unpack;
  GLboolean blend, depth, cull, scissor, stencil;
  GLint attr_on[2];
  GLfloat clear[4];
} GlState;

static void gl_save(GlState *s) {
  glGetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_tex);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex[i]);
  }
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buf);
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s->elem_buf);
  glGetIntegerv(GL_VIEWPORT, s->viewport);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear);
  s->blend = glIsEnabled(GL_BLEND);
  s->depth = glIsEnabled(GL_DEPTH_TEST);
  s->cull = glIsEnabled(GL_CULL_FACE);
  s->scissor = glIsEnabled(GL_SCISSOR_TEST);
  s->stencil = glIsEnabled(GL_STENCIL_TEST);
  glGetVertexAttribiv((GLuint)g_a_pos, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attr_on[0]);
  glGetVertexAttribiv((GLuint)g_a_uv, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attr_on[1]);
}

static void set_cap(GLenum cap, GLboolean on) {
  if (on)
    glEnable(cap);
  else
    glDisable(cap);
}

static void gl_restore(const GlState *s) {
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex[i]);
  }
  glActiveTexture((GLenum)s->active_tex);
  glUseProgram((GLuint)s->prog);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buf);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s->elem_buf);
  glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
  glPixelStorei(GL_UNPACK_ALIGNMENT, s->unpack);
  glClearColor(s->clear[0], s->clear[1], s->clear[2], s->clear[3]);
  set_cap(GL_BLEND, s->blend);
  set_cap(GL_DEPTH_TEST, s->depth);
  set_cap(GL_CULL_FACE, s->cull);
  set_cap(GL_SCISSOR_TEST, s->scissor);
  set_cap(GL_STENCIL_TEST, s->stencil);
  if (s->attr_on[0])
    glEnableVertexAttribArray((GLuint)g_a_pos);
  else
    glDisableVertexAttribArray((GLuint)g_a_pos);
  if (s->attr_on[1])
    glEnableVertexAttribArray((GLuint)g_a_uv);
  else
    glDisableVertexAttribArray((GLuint)g_a_uv);
}

/* One plane: rows copied tight when the decoder pads them (GLES 2 has no
 * GL_UNPACK_ROW_LENGTH). */
static uint8_t *g_tight;
static size_t g_tight_cap;

static void upload(int i, const uint8_t *data, int linesize, int w, int h, int realloc_tex) {
  const uint8_t *src = data;
  if (linesize != w) {
    size_t need = (size_t)w * (size_t)h;
    if (need > g_tight_cap) {
      free(g_tight);
      g_tight = malloc(need);
      g_tight_cap = g_tight ? need : 0;
      if (!g_tight)
        return;
    }
    for (int y = 0; y < h; y++)
      memcpy(g_tight + (size_t)y * w, data + (size_t)y * linesize, (size_t)w);
    src = g_tight;
  }
  glActiveTexture(GL_TEXTURE0 + i);
  glBindTexture(GL_TEXTURE_2D, g_tex[i]);
  if (realloc_tex)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, src);
  else
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, src);
}

static void draw(const AVFrame *f, int realloc_tex) {
  int ww = 1280, wh = 720;
  dcr_window_size(&ww, &wh);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  upload(0, f->data[0], f->linesize[0], f->width, f->height, realloc_tex);
  upload(1, f->data[1], f->linesize[1], (f->width + 1) / 2, (f->height + 1) / 2, realloc_tex);
  upload(2, f->data[2], f->linesize[2], (f->width + 1) / 2, (f->height + 1) / 2, realloc_tex);

  /* letterbox: the picture's aspect (sample aspect included) in the window */
  double sar = f->sample_aspect_ratio.num > 0 ? av_q2d(f->sample_aspect_ratio) : 1.0;
  double pic = (double)f->width * sar / (double)f->height, win = (double)ww / (double)wh;
  float sx = 1.0f, sy = 1.0f;
  if (pic > win)
    sy = (float)(win / pic);
  else
    sx = (float)(pic / win);
  const GLfloat quad[] = {-sx, -sy, 0, 1, sx, -sy, 1, 1, -sx, sy, 0, 0, sx, sy, 1, 0};

  glViewport(0, 0, ww, wh);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(g_prog);
  glUniform1i(glGetUniformLocation(g_prog, "t_y"), 0);
  glUniform1i(glGetUniformLocation(g_prog, "t_u"), 1);
  glUniform1i(glGetUniformLocation(g_prog, "t_v"), 2);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray((GLuint)g_a_pos);
  glEnableVertexAttribArray((GLuint)g_a_uv);
  glVertexAttribPointer((GLuint)g_a_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
  glVertexAttribPointer((GLuint)g_a_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray((GLuint)g_a_pos);
  glDisableVertexAttribArray((GLuint)g_a_uv);
}

/* ---------------------------------------------------------------- sound
 * MP4s interleave their sound in runs (half a second of AAC packets in a
 * row): written straight from the read loop, each blocking write would hold
 * the pictures back. So the decoded sound goes into a ring, and a thread of
 * its own plays it at the device's pace (mc3_audio_write_video blocks). */
#define RING_FRAMES (48000 * 2) /* two seconds of stereo frames */
static int16_t g_ring[RING_FRAMES * 2];
static volatile unsigned g_ring_r, g_ring_w; /* frame counts; w - r = queued */
static volatile int g_ring_rate, g_audio_run, g_audio_flush;
static Thread g_audio_thread;

static void audio_thread(void *arg) {
  (void)arg;
  static int16_t chunk[1024 * 2];
  while (g_audio_run) {
    unsigned q = g_ring_w - g_ring_r;
    if (q == 0) {
      if (g_audio_flush)
        break;
      svcSleepThread(5000000LL);
      continue;
    }
    unsigned n = q > 1024 ? 1024 : q;
    for (unsigned i = 0; i < n; i++) {
      unsigned k = (g_ring_r + i) % RING_FRAMES;
      chunk[i * 2] = g_ring[k * 2];
      chunk[i * 2 + 1] = g_ring[k * 2 + 1];
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_ring_r += n;
    mc3_audio_write_video(chunk, (int)n, 2, g_ring_rate);
  }
}

static int audio_start(void) {
  g_ring_r = g_ring_w = 0;
  g_audio_run = 1;
  g_audio_flush = 0;
  if (R_FAILED(threadCreate(&g_audio_thread, audio_thread, NULL, NULL, 0x8000, 0x2A, -2)))
    return -1;
  if (R_FAILED(threadStart(&g_audio_thread))) {
    threadClose(&g_audio_thread);
    return -1;
  }
  return 0;
}

/* played to the end (flush) or cut off */
static void audio_stop(int flush) {
  if (flush)
    g_audio_flush = 1;
  else
    g_audio_run = 0;
  threadWaitForExit(&g_audio_thread);
  threadClose(&g_audio_thread);
  g_audio_run = 0;
}

static float sample_at(const AVFrame *f, int ch, int i) {
  int nch = f->ch_layout.nb_channels;
  switch (f->format) {
  case AV_SAMPLE_FMT_FLTP: return ((const float *)f->extended_data[ch])[i];
  case AV_SAMPLE_FMT_FLT: return ((const float *)f->extended_data[0])[i * nch + ch];
  case AV_SAMPLE_FMT_S16P: return ((const int16_t *)f->extended_data[ch])[i] / 32768.0f;
  case AV_SAMPLE_FMT_S16: return ((const int16_t *)f->extended_data[0])[i * nch + ch] / 32768.0f;
  default: return 0.0f;
  }
}

/* Into the ring; waits (rarely) while it is full. */
static void queue_audio(const AVFrame *f) {
  int n = f->nb_samples, nch = f->ch_layout.nb_channels;
  if (n <= 0 || nch <= 0)
    return;
  g_ring_rate = f->sample_rate;
  for (int i = 0; i < n; i++) {
    while (g_ring_w - g_ring_r >= RING_FRAMES && g_audio_run)
      svcSleepThread(2000000LL);
    unsigned k = g_ring_w % RING_FRAMES;
    for (int c = 0; c < 2; c++) {
      float v = sample_at(f, nch > 1 ? c : 0, i) * 32767.0f;
      g_ring[k * 2 + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_ring_w++;
  }
}

/* ---------------------------------------------------------------- skip */
static PadState g_vpad;
static int g_vpad_ready;

static int skip_pressed(void) {
  if (!g_vpad_ready) {
    padInitializeDefault(&g_vpad);
    g_vpad_ready = 1;
  }
  padUpdate(&g_vpad);
  if (padGetButtonsDown(&g_vpad) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus))
    return 1;
  static int was_touching;
  HidTouchScreenState t;
  int touching = hidGetTouchScreenStates(&t, 1) && t.count > 0;
  int tap = was_touching && !touching;
  was_touching = touching;
  return tap;
}

/* ---------------------------------------------------------------- play */
static uint64_t now_us(void) { return armTicksToNs(armGetSystemTick()) / 1000; }

static AVCodecContext *open_decoder(AVStream *st) {
  const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
  if (!c)
    return NULL;
  AVCodecContext *ctx = avcodec_alloc_context3(c);
  if (!ctx)
    return NULL;
  if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, c, NULL) < 0) {
    avcodec_free_context(&ctx);
    return NULL;
  }
  return ctx;
}

int mc3_video_play(const char *android_path) {
  char real_buf[DCR_PATH_MAX];
  const char *real = dcr_translate_path(android_path, real_buf, sizeof real_buf);
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, real, NULL, NULL) < 0) {
    debugPrintf("[video] %s: cannot open\n", real);
    return -1;
  }
  if (avformat_find_stream_info(fmt, NULL) < 0) {
    avformat_close_input(&fmt);
    return -1;
  }
  int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  AVCodecContext *vd = vi >= 0 ? open_decoder(fmt->streams[vi]) : NULL;
  AVCodecContext *ad = ai >= 0 ? open_decoder(fmt->streams[ai]) : NULL;
  if (!vd || gl_setup() != 0) {
    debugPrintf("[video] %s: no playable picture (decoder %s)\n", real, vd ? "ok" : "missing");
    avcodec_free_context(&vd);
    avcodec_free_context(&ad);
    avformat_close_input(&fmt);
    return -1;
  }
  debugPrintf("[video] %s: %dx%d %s%s%s\n", strrchr(real, '/') ? strrchr(real, '/') + 1 : real,
              vd->width, vd->height, vd->codec->name, ad ? ", sound " : "", ad ? ad->codec->name : "");

  GlState saved;
  gl_save(&saved);
  if (ad && audio_start() != 0) {
    debugPrintf("[video] no sound thread: playing without sound\n");
    avcodec_free_context(&ad);
  }
  if (ad)
    mc3_audio_video_owner(1);

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVRational vtb = fmt->streams[vi]->time_base;
  uint64_t t0 = 0;
  int64_t first_pts = AV_NOPTS_VALUE;
  int tex_w = 0, tex_h = 0, frames = 0, dropped = 0, skipped = 0, bad_fmt = 0, eof = 0;

  while (!skipped && !rt_exit_requested() && appletMainLoop()) {
    rt_applet_poll();
    if (skip_pressed()) {
      skipped = 1;
      break;
    }
    if (!eof) {
      int r = av_read_frame(fmt, pkt);
      if (r < 0) {
        eof = 1;
        avcodec_send_packet(vd, NULL); /* drain */
        if (ad)
          avcodec_send_packet(ad, NULL);
      } else if (pkt->stream_index == ai && ad) {
        if (avcodec_send_packet(ad, pkt) == 0)
          while (avcodec_receive_frame(ad, frame) == 0)
            queue_audio(frame);
        av_packet_unref(pkt);
        continue;
      } else if (pkt->stream_index == vi) {
        avcodec_send_packet(vd, pkt);
        av_packet_unref(pkt);
      } else {
        av_packet_unref(pkt);
        continue;
      }
    }
    int got = 0;
    while (avcodec_receive_frame(vd, frame) == 0) {
      got = 1;
      if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
        if (!bad_fmt++)
          debugPrintf("[video] pixel format %d not handled: picture skipped\n", frame->format);
        continue;
      }
      int64_t pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE)
        pts = first_pts == AV_NOPTS_VALUE ? 0 : first_pts;
      if (first_pts == AV_NOPTS_VALUE) {
        first_pts = pts;
        t0 = now_us();
      }
      uint64_t due = t0 + (uint64_t)(av_q2d(vtb) * (double)(pts - first_pts) * 1e6);
      uint64_t now = now_us();
      if (now + 1000 < due)
        svcSleepThread((int64_t)(due - now) * 1000);
      else if (now > due + 100000) { /* over 100 ms late: decode, do not draw */
        dropped++;
        continue;
      }
      int realloc_tex = frame->width != tex_w || frame->height != tex_h;
      tex_w = frame->width;
      tex_h = frame->height;
      draw(frame, realloc_tex);
      mc3_gl_swap();
      frames++;
    }
    if (eof && !got)
      break;
  }
  if (ad && eof && !skipped) /* the decoder's last frames */
    while (avcodec_receive_frame(ad, frame) == 0)
      queue_audio(frame);
  if (ad)
    audio_stop(eof && !skipped); /* played out, or cut off (skip, exit) */

  debugPrintf("[video] %d pictures shown, %d late%s\n", frames, dropped, skipped ? ", skipped" : "");
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&vd);
  avcodec_free_context(&ad);
  avformat_close_input(&fmt);
  if (ad)
    mc3_audio_video_owner(0);
  gl_restore(&saved);
  return 0;
}

#endif /* MC3_VIDEO */
