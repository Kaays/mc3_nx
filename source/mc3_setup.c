/* mc3_setup.c -- the port's own part of the first start, and file tracing.
 *
 * The briefing videos: on Android the game's installer unpacks the patch
 * OBB (a zip of the mission briefings, Briefing_Mxx.mp4) into its files
 * folder, and the game opens them from there as <SD folder>/data/briefing/
 * (getSDFolder: /sdcard/Android/data/<pkg>/files/, which the runtime keeps in
 * <game folder>/external/files/). The PS Vita port made the player unpack
 * them by hand and refused to start without Briefing_M01.mp4; here the setup
 * does it once, from obb/<patch OBB>, and again only when that file changes.
 * Without them the game's file layer falls into a path that dereferences a
 * NULL archive name (libModernCombat3.so+0x842ec, strstr(NULL, ".obfs")).
 *
 * The expansion files themselves stay zips: the game reads them in place.
 * MIT.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "port_config.h"
#include "rt_boot.h"
#include "util.h"

#include "mc3.h"

#define BRIEFING_DIR "external/files/data/briefing"
#define STAMP_KEY    "briefing"

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

void mc3_setup_obb(RtSetupCtx *ctx) {
  char obb[DCR_PATH_MAX], main_obb[DCR_PATH_MAX];
  snprintf(obb, sizeof obb, "%s/obb/" MC3_OBB_PATCH, dcr_game_root());
  snprintf(main_obb, sizeof main_obb, "%s/obb/" MC3_OBB_MAIN, dcr_game_root());
  if (rt_file_size(main_obb) <= 0 || rt_file_size(obb) <= 0)
    fatal_error("The game's OBB files are missing.\n\n%s", port_apk_help());

  unsigned long obb_size = (unsigned long)rt_file_size(obb), crc = 0, count = 0;
  char first[DCR_PATH_MAX];
  rt_root_path(first, sizeof first, BRIEFING_DIR "/Briefing_M01.mp4");
  if (rt_setup_stamp_get(STAMP_KEY, &crc, &count) && crc == obb_size && count > 0 && exists(first))
    return; /* done, and the OBB has not changed */

  mz_zip_archive zip;
  memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_file(&zip, obb, 0))
    fatal_error("%s is not a readable zip.\n\n%s", obb, port_apk_help());
  unsigned n = mz_zip_reader_get_num_files(&zip);
  uint64_t total = 0, done = 0;
  for (unsigned i = 0; i < n; i++) {
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat(&zip, i, &st) && !st.m_is_directory)
      total += st.m_uncomp_size;
  }
  debugPrintf("[mc3] unpacking the briefing videos: %u entries, %llu MB, from %s\n", n,
              (unsigned long long)(total >> 20), MC3_OBB_PATCH);
  unsigned made = 0;
  for (unsigned i = 0; i < n; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory)
      continue;
    const char *name = st.m_filename;
    while (*name == '/')
      name++;
    if (strstr(name, ".."))
      continue;
    char dst[DCR_PATH_MAX], rel[DCR_PATH_MAX];
    snprintf(rel, sizeof rel, BRIEFING_DIR "/%s", name);
    rt_root_path(dst, sizeof dst, rel);
    int p0 = ctx->p0 + (int)((uint64_t)(ctx->p1 - ctx->p0) * done / (total ? total : 1));
    done += st.m_uncomp_size;
    int p1 = ctx->p0 + (int)((uint64_t)(ctx->p1 - ctx->p0) * done / (total ? total : 1));
    if (rt_setup_extract_entry(&zip, (int)i, dst, "Unpacking the briefing videos", p0, p1) != 0)
      fatal_error("Could not unpack %s from %s (SD card full?).", name, MC3_OBB_PATCH);
    made++;
  }
  mz_zip_reader_end(&zip);
  if (!exists(first))
    debugPrintf("[mc3] note: no Briefing_M01.mp4 in %s (unexpected layout)\n", MC3_OBB_PATCH);
  rt_setup_stamp_set(STAMP_KEY, obb_size, made);
  debugPrintf("[mc3] %u files unpacked to %s/\n", made, BRIEFING_DIR);
}

/* ---------------------------------------------------------------- tracing
 * While the port is brought up: every path the game opens or stats goes to
 * debug.log ([path], [io] fopen/open/stat lines of the runtime), so a
 * missing file names itself. [debug] trace_files = false turns it off. */
int g_mc3_trace_files = 1;

int dcr_path_traced(const char *p) {
  if (!p)
    return 0;
  if (g_mc3_trace_files)
    return 1;
  return strstr(p, ".apk") != NULL;
}
