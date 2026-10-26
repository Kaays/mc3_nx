/* port_config.h -- Modern Combat 3: Fallen Nation for Switch (android32).
 *
 * The Android release 1.1.7g (com.gameloft.android.ANMP.GloftM3HM),
 * lib/armeabi-v7a/libModernCombat3.so: Gameloft's GLF engine, GLES 2,
 * softfp. One native library; the Java side is answered by mc3_jni.c.
 * MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#define PORT_TITLE   "Modern Combat 3"
#define PORT_NAME    "mc3_nx"
#define PORT_PACKAGE "com.gameloft.android.ANMP.GloftM3HM"

/* The one library, and the build it must be: the hard-coded addresses in
 * mc3_main.c (DRM flags) belong to this exact file. Its CRC-32 is the APK
 * entry's, which the setup stamps (no extra read at boot). */
#define MC3_LIB          "libModernCombat3.so"
#define MC3_LIB_CRC32    0x845949abUL
#define MC3_LIB_SIZE     9766612UL
#define MC3_VERSION      "1.1.7g"

/* The expansion files, as the game's own installer names them. They stay
 * zips: the engine reads them itself (getZipName / getPatchFileLocation). */
#define MC3_OBB_MAIN     "main.1120." PORT_PACKAGE ".obb"
#define MC3_OBB_PATCH    "patch.11428." PORT_PACKAGE ".obb"

/* The game's mixer hands AudioTrack 44.1 kHz stereo s16 (mc3_audio.c). */
#define RT_OPENSLES 0

#endif /* PORT_CONFIG_H */
