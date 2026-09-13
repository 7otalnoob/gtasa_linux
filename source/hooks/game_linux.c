/* Linux platform hooks for the Android ARM64 game.
 *
 * Derived from the MIT-licensed gtasa_nx thread/platform hooks (game.c).
 * Do NOT apply that file's instruction-offset gameplay patches here: those
 * offsets target a different game build. The known v2.11.264 BuildPixelSource
 * offset is strcat, not the specular-lighting instruction; patching it corrupts
 * GLSL. The Linux path uses symbol-only platform hooks for v2.11.311 instead.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hooks.h"
#include "../config.h"
#include "../jni_fake.h"
#include "../so_util.h"
#include "../util.h"

extern so_module game_mod;
extern void linux_release_sdl_context(void);



typedef struct {
  void *(*func)(void *);
  void *arg;
  char name[16];
} GameThreadStart;

static void *game_thread_start(void *arg) {
  GameThreadStart start = *(GameThreadStart *)arg;
  free(arg);
  pthread_setname_np(pthread_self(), start.name);
  if (strcmp(start.name, "RenderQueue") == 0)
    linux_release_sdl_context();
  thread_registry_add();
  /* Leave glibc's thread pointer intact. The game obtains JNIEnv through the
   * hook below and pthread TLS through our Bionic import adapters. */
  return start.func(start.arg);
}

static void *current_jni_env(void) {
  return fake_env;
}

/* NVThreadSpawnJNIThread(long*, const Android pthread_attr_t*, const char*,
 *                        void* (*)(void*), void*)
 * Android attributes are not layout-compatible with glibc. Use Linux defaults
 * rather than reinterpret them; retain a joinable native pthread_t handle.
 */
static int spawn_jni_thread(long *tid, const void *attr, const char *name,
                            void *(*func)(void *), void *arg) {
  (void)attr;
  _Static_assert(sizeof(pthread_t) == sizeof(long), "Android thread handle size");
  GameThreadStart *start = calloc(1, sizeof(*start));
  if (!start)
    return ENOMEM;
  start->func = func;
  start->arg = arg;
  strlcpy(start->name, name ? name : "game", sizeof(start->name));
  pthread_t thread;
  int err = pthread_create(&thread, NULL, game_thread_start, start);
  if (err) {
    free(start);
    return err;
  }
  if (tid)
    memcpy(tid, &thread, sizeof(thread));
  debugPrintf("thread: started %s\n", name ? name : "game");
  return 0;
}

static int screen_get_width(void) { return screen_width; }
static int screen_get_height(void) { return screen_height; }

void patch_game(void) {
  /* Whole-function replacements only, no displaced-instruction trampolines.
   * These platform entry signatures are present in the target v2.11.311
   * Android library; no version-sensitive gameplay offsets are applied. */
  const DynLibFunction hooks[] = {
    {"_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_",
     (uintptr_t)spawn_jni_thread},
    {"_Z24NVThreadGetCurrentJNIEnvv", (uintptr_t)current_jni_env},
    {"_Z17OS_ScreenGetWidthv", (uintptr_t)screen_get_width},
    {"_Z18OS_ScreenGetHeightv", (uintptr_t)screen_get_height},
  };
  for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
    hook_arm64(so_find_addr(&game_mod, hooks[i].symbol), hooks[i].func);

  uintptr_t cloud_saves = so_try_find_addr_rx(&game_mod, "UseCloudSaves");
  if (cloud_saves)
    *(uint8_t *)cloud_saves = 0;
  debugPrintf("hooks: Linux platform only; version-sensitive gameplay offsets disabled\n");
}
