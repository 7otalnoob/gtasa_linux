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

static int is_aml_image(const so_module *mod) {
  int loads = 0;
  if (mod->elf_hdr->e_type != ET_EXEC ||
      mod->elf_hdr->e_machine != EM_AARCH64)
    return 0;
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD) {
      loads++;
      if (p->p_offset != 0 || p->p_vaddr != 0 || p->p_filesz > mod->so_size)
        return 0;
    } else if (p->p_type == PT_DYNAMIC || p->p_type == PT_INTERP ||
               p->p_type == PT_TLS)
      return 0;
  }
  return loads == 1;
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

  if (mprotect(module->so.load_base, module->so.load_size,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    debugPrintf("AML: mprotect failed for %s: %s\n", path, strerror(errno));
    so_unload(&module->so);
    return -1;
  }

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

void aml_load_mods(const char *directory, so_module *game) {
  (void)game;
  const char *enabled = getenv("GTASA_AML_MODS");
  if (enabled && (!strcmp(enabled, "0") || !strcmp(enabled, "false")))
    return;

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
