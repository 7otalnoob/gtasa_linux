#define _GNU_SOURCE

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "aml_mod.h"
#include "imports.h"
#include "util.h"

#define AML_MAX_MODULES 32
#define AML_MAX_SIZE (16u * 1024u * 1024u)

struct aml_mod_info {
  char guid[48];
  char name[48];
  char version[24];
  char author[24];
};

typedef const void *(*aml_get_info_fn)(void);
typedef void (*aml_callback_fn)(void);

typedef struct {
  so_module so;
  char path[PATH_MAX];
} aml_module;

static aml_module g_modules[AML_MAX_MODULES];
static size_t g_module_count;

// Real AML mods (unlike the flat ET_EXEC images this originally assumed)
// are ordinary Android shared objects: ET_DYN, with a PT_DYNAMIC segment,
// linked against libc/libm/libdl/liblog and relocated the same way as the
// game image itself. PT_INTERP (a dynamic executable, not a library) and
// PT_TLS (ELF thread-locals; unsupported by so_relocate) are still rejected.
static int is_aml_image(const so_module *mod) {
  if (mod->elf_hdr->e_type != ET_DYN || mod->elf_hdr->e_machine != EM_AARCH64)
    return 0;
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_INTERP || p->p_type == PT_TLS)
      return 0;
  }
  return 1;
}

static int has_suffix(const char *name, const char *suffix) {
  size_t n = strlen(name), s = strlen(suffix);
  return n >= s && strcmp(name + n - s, suffix) == 0;
}

static int load_one(const char *path) {
  if (g_module_count >= AML_MAX_MODULES) {
    debugPrintf("AML: module limit reached, skipping %s\n", path);
    return -1;
  }

  aml_module *module = &g_modules[g_module_count];
  memset(module, 0, sizeof(*module));
  if (so_load(&module->so, path, NULL, AML_MAX_SIZE) < 0) {
    debugPrintf("AML: failed to map %s\n", path);
    return -1;
  }
  if (!is_aml_image(&module->so)) {
    debugPrintf("AML: unsupported image format, skipping %s\n", path);
    so_unload(&module->so);
    return -1;
  }

  // Same pipeline main_linux.c runs for the game image itself: resolve
  // undefined imports (libc/libm/libdl/liblog, plus our dlopen/dlsym shim
  // and, for anything the mod calls directly on the game, game_mod's own
  // exports via cross-module lookup), lay down final page protections, then
  // run the mod's C++ static initializers.
  so_relocate(&module->so);
  so_resolve(&module->so, dynlib_functions, dynlib_numfunctions, 1);
  so_finalize(&module->so);
  so_flush_caches(&module->so);
  so_execute_init_array(&module->so);
  so_free_temp(&module->so);

  aml_get_info_fn get_info = (aml_get_info_fn)
      so_try_find_addr_rx(&module->so, "__GetModInfo");
  aml_callback_fn pre_load = (aml_callback_fn)
      so_try_find_addr_rx(&module->so, "OnModPreLoad");
  aml_callback_fn on_load = (aml_callback_fn)
      so_try_find_addr_rx(&module->so, "OnModLoad");
  aml_callback_fn all_loaded = (aml_callback_fn)
      so_try_find_addr_rx(&module->so, "OnAllModsLoaded");
  if (!get_info || !pre_load || !on_load || !all_loaded) {
    debugPrintf("AML: missing required exports, skipping %s\n", path);
    so_unload(&module->so);
    return -1;
  }

  const struct aml_mod_info *info = get_info();
  if (!info) {
    debugPrintf("AML: __GetModInfo returned NULL for %s\n", path);
    so_unload(&module->so);
    return -1;
  }
  strlcpy(module->path, path, sizeof(module->path));
  debugPrintf("AML: loading %s (%s %s by %s)\n", path, info->name,
              info->version, info->author);
  pre_load();
  on_load();
  (void)all_loaded;
  g_module_count++;
  return 0;
}

// so_finalize() deliberately leaves the game's PF_X segments RX and never
// asks Linux for writable+executable memory. AML mods, however, are built
// against Android's loader, where AML itself leaves the game image writable,
// so they patch instructions in place without calling mprotect first (most
// of them do not even import it). Re-arm those segments as RWX before any
// mod runs, or the first patch write faults and takes the process down
// before the game loop starts. Failure is not fatal: a hardened kernel that
// refuses W^X simply means code-patching mods will not work.
static void unprotect_game_text(so_module *game) {
  if (!game || !game->load_virtbase)
    return;
  for (int i = 0; i < game->phnum; i++) {
    const Elf64_Phdr *p = &game->phdr[i];
    if (p->p_type != PT_LOAD || (p->p_flags & PF_X) != PF_X)
      continue;
    uintptr_t start = (uintptr_t)game->load_virtbase + p->p_vaddr;
    uintptr_t end = ALIGN_MEM(start + p->p_memsz, 0x1000);
    start &= ~(uintptr_t)0xfff;
    if (mprotect((void *)start, end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
      debugPrintf("AML: mprotect RWX on game text failed: %s "
                  "(code-patching mods will crash)\n", strerror(errno));
  }
}

void aml_load_mods(const char *directory, so_module *game) {
  const char *enabled = getenv("GTASA_AML_MODS");
  if (enabled && (!strcmp(enabled, "0") || !strcmp(enabled, "false")))
    return;

  unprotect_game_text(game);

  DIR *dir = opendir(directory);
  if (!dir)
    return;
  struct dirent *entry;
  char **names = NULL;
  size_t count = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.' || !has_suffix(entry->d_name, ".so"))
      continue;
    char **next = realloc(names, (count + 1) * sizeof(*names));
    if (!next)
      break;
    names = next;
    names[count++] = strdup(entry->d_name);
  }
  closedir(dir);

  for (size_t i = 0; i < count; i++)
    for (size_t j = i + 1; j < count; j++)
      if (strcmp(names[i], names[j]) > 0) {
        char *tmp = names[i]; names[i] = names[j]; names[j] = tmp;
      }
  for (size_t i = 0; i < count; i++) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", directory, names[i]) <
        (int)sizeof(path))
      load_one(path);
    free(names[i]);
  }
  for (size_t i = 0; i < g_module_count; i++) {
    aml_callback_fn all_loaded = (aml_callback_fn)
        so_try_find_addr_rx(&g_modules[i].so, "OnAllModsLoaded");
    if (all_loaded)
      all_loaded();
  }
  free(names);
}
