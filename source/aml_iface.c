/* aml_iface.c -- minimal IAML compatibility shim
 *
 * Real AML mods (the AndroidModLoader ecosystem, https://github.com/RusJJ/
 * AndroidModLoader, MIT) bootstrap themselves with a highest-priority global
 * constructor that does:
 *
 *   void* module = dlopen("libAML.so", RTLD_NOW);
 *   GetInterfaceFn fn = (GetInterfaceFn)dlsym(module, "GetInterface");
 *   IAML* aml = (IAML*)fn("AMLInterface");
 *
 * with NO null-check on any of those three steps (see mod/amlmod.h's
 * MYMOD() macro and mod/interface.h's GetInterface()). On this port there is
 * no real libAML.so, so that chain silently produced a NULL `aml` pointer,
 * and the mod's own code then called through it -- an immediate crash
 * before OnModPreLoad/OnModLoad even ran.
 *
 * `aml` is a pointer to a pure-interface C++ class (IAML, mod/iaml.h):
 * single inheritance, no data members, so at the ABI level it is just a
 * pointer to a pointer to a 130-entry vtable of function pointers, in
 * exact declaration order (Itanium C++ ABI). We build that vtable by hand
 * in plain C: no C++ compiler involvement needed, since nothing here ever
 * *calls* these through a real C++ IAML* -- the mod's own compiled code
 * does the indexed vtable dispatch itself, exactly as it would against the
 * real libAML.so.
 *
 * Counted directly from mod/iaml.h (AML 1.4.1's IAML, 130 virtuals total,
 * "Inlines" section excluded since those are non-virtual helpers that just
 * call back into the real virtuals). Get the count and order wrong and a
 * mod calling into a later slot reads a mismatched function pointer instead
 * of an out-of-bounds crash -- worse than the crash we started with -- so
 * every slot below is filled, even the ones that are just safe no-ops.
 *
 * Only a small subset is wired to real behaviour: the memory/lib-lookup
 * primitives (GetLib, GetSym, Write, Read, Unprot, PlaceNOP, PlaceJMP/Hook,
 * HasMod, GetCurrentGame, path getters, IsMainThread, file helpers, CPU
 * count). Everything else is a shape-matched stub (returns 0/NULL/false/
 * empty string, or does nothing for void) so a mod that happens to call a
 * feature we haven't implemented gets an inert response instead of a wild
 * jump. If a mod's behaviour looks wrong rather than crashing, the next
 * step is finding out which of these stubs it actually needs made real.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aml_iface.h"
#include "config.h"
#include "so_util.h"
#include "util.h"

extern so_module game_mod;

// -- helpers --------------------------------------------------------------

static int is_game_lib_name(const char *name) {
  if (!name)
    return 0;
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  return strcmp(base, SO_NAME) == 0 || strcmp(base, "libGTASA.so") == 0;
}

// Every real IAML method takes the (hidden) `this` pointer first; we never
// dereference it (the shim carries no state), it just has to be present so
// the argument registers behind it line up with what the mod passed.
typedef void *This;

// -- functional subset ------------------------------------------------------

static const char *iaml_GetCurrentGame(This t) { (void)t; return "gtasa"; }
static const char *iaml_GetRealCurrentGame(This t) { (void)t; return "gtasa"; }
static const char *iaml_GetConfigPath(This t) { (void)t; return "."; }
static const char *iaml_GetDataPath(This t) { (void)t; return "."; }
static const char *iaml_GetAndroidDataPath(This t) { (void)t; return "."; }
static const char *iaml_GetAndroidDataRootPath(This t) { (void)t; return "."; }
static const char *iaml_GetInternalPath(This t) { (void)t; return "."; }
static const char *iaml_GetInternalModsPath(This t) { (void)t; return "mods"; }
static const char *iaml_GetNativeLibsPath(This t) { (void)t; return "."; }
static const char *iaml_GetApkPath(This t) { (void)t; return ""; }
static const char *iaml_GetFeatures(This t) { (void)t; return ""; }

static bool iaml_HasMod(This t, const char *guid) {
  (void)t; (void)guid;
  return false; // we don't track a mod list; safest default
}
static bool iaml_HasModOfVersion(This t, const char *guid, const char *ver) {
  (void)t; (void)guid; (void)ver;
  return false;
}
static bool iaml_HasModOfBiggerVersion(This t, const char *guid, const char *ver) {
  (void)t; (void)guid; (void)ver;
  return false;
}

static uintptr_t iaml_GetLib(This t, const char *lib) {
  (void)t;
  if (is_game_lib_name(lib))
    return (uintptr_t)game_mod.load_virtbase;
  return 0;
}
static uintptr_t iaml_GetLibLength(This t, const char *lib) {
  (void)t;
  if (is_game_lib_name(lib))
    return (uintptr_t)game_mod.load_size;
  return 0;
}
static void *iaml_GetLibHandle_name(This t, const char *lib) {
  (void)t;
  if (is_game_lib_name(lib))
    return game_mod.load_virtbase;
  return NULL;
}
static void *iaml_GetLibHandle_addr(This t, uintptr_t addr) {
  (void)t;
  uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  if (addr >= base && addr < base + game_mod.load_size)
    return game_mod.load_virtbase;
  return NULL;
}
// Both GetSym overloads: a "handle" here is always game_mod.load_virtbase
// (the only library this shim knows about), so both boil down to the same
// lookup against the game's own dynamic symbol table.
static uintptr_t iaml_GetSym_handle(This t, void *handle, const char *sym) {
  (void)t; (void)handle;
  return so_try_find_addr_rx(&game_mod, sym);
}
static uintptr_t iaml_GetSym_addr(This t, uintptr_t lib_addr, const char *sym) {
  (void)t; (void)lib_addr;
  return so_try_find_addr_rx(&game_mod, sym);
}

// -- pattern scanning / pointer chains --------------------------------------
//
// Matches AndroidModLoader's own src/aml.cpp ParsePattern/ComparePattern
// exactly: pattern is whitespace-separated hex byte tokens, "?" or "??" is
// a wildcard byte. This is almost certainly the missing piece for mods
// that load and run cleanly but don't visibly do anything -- they use
// PatternScan to locate their hook targets by byte signature (so they
// still work across small game-version differences) instead of a fixed
// offset, and gracefully skip installing the hook when it returns 0.
#define MAX_PATTERN_BYTES 512

static size_t parse_pattern(const char *pattern, int *out, size_t max) {
  size_t n = 0;
  const char *p = pattern;
  while (p && *p && n < max) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    size_t len = (size_t)(p - start);
    if ((len == 1 && start[0] == '?') || (len == 2 && start[0] == '?' && start[1] == '?')) {
      out[n++] = -1;
    } else {
      char buf[8];
      if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
      memcpy(buf, start, len);
      buf[len] = 0;
      out[n++] = (int)strtoul(buf, NULL, 16);
    }
  }
  return n;
}

static int compare_pattern(const uint8_t *data, const int *pattern, size_t len) {
  for (size_t i = 0; i < len; i++)
    if (pattern[i] != -1 && data[i] != (uint8_t)pattern[i])
      return 0;
  return 1;
}

static uintptr_t iaml_PatternScan_range(This t, const char *pattern, uintptr_t libStart, uintptr_t scanLen) {
  (void)t;
  if (!pattern || !libStart || !scanLen)
    return 0;
  int parsed[MAX_PATTERN_BYTES];
  size_t patLen = parse_pattern(pattern, parsed, MAX_PATTERN_BYTES);
  if (!patLen || scanLen < patLen)
    return 0;
  const uint8_t *scanStart = (const uint8_t *)libStart;
  size_t searchLen = (size_t)scanLen - patLen;
  for (size_t i = 0; i <= searchLen; i++)
    if (compare_pattern(scanStart + i, parsed, patLen))
      return (uintptr_t)(scanStart + i);
  return 0;
}

static uintptr_t iaml_PatternScan_lib(This t, const char *pattern, const char *soLib) {
  uintptr_t libStart = iaml_GetLib(t, soLib);
  uintptr_t scanLen = iaml_GetLibLength(t, soLib);
  return iaml_PatternScan_range(t, pattern, libStart, scanLen);
}

static bool iaml_ComparePattern(This t, uintptr_t addr, const char *pattern) {
  (void)t;
  if (!addr || !pattern)
    return false;
  int parsed[MAX_PATTERN_BYTES];
  size_t patLen = parse_pattern(pattern, parsed, MAX_PATTERN_BYTES);
  if (!patLen)
    return false;
  return compare_pattern((const uint8_t *)addr, parsed, patLen) != 0;
}

static uintptr_t iaml_ReadPointerChain(This t, uintptr_t baseAddr, const int *offsets, size_t count) {
  (void)t;
  uintptr_t cur = baseAddr;
  for (size_t i = 0; i < count; i++) {
    if (!cur)
      return 0;
    cur = *(uintptr_t *)cur;
    cur += (uintptr_t)offsets[i];
  }
  return cur;
}

// The game's text segments were already made RWX up front by
// unprotect_game_text() (aml_mod.c) before any mod runs, specifically so
// this kind of direct patch works -- so Unprot is a no-op success here.
static int iaml_Unprot(This t, uintptr_t addr, size_t len) {
  (void)t; (void)addr; (void)len;
  return 0;
}
static void iaml_Write(This t, uintptr_t dest, uintptr_t src, size_t size) {
  (void)t;
  if (dest && src && size)
    memcpy((void *)dest, (void *)src, size);
}
static void iaml_Read(This t, uintptr_t src, uintptr_t dest, size_t size) {
  (void)t;
  if (dest && src && size)
    memcpy((void *)dest, (void *)src, size);
}
static int iaml_PlaceNOP(This t, uintptr_t addr, size_t count) {
  (void)t;
  if (!addr)
    return -1;
  uint32_t *p = (uint32_t *)addr;
  for (size_t i = 0; i < count; i++)
    p[i] = 0xd503201fu; // AArch64 NOP
  return 0;
}
static int iaml_PlaceNOP4(This t, uintptr_t addr, size_t count) {
  return iaml_PlaceNOP(t, addr, count);
}
static int iaml_PlaceJMP(This t, uintptr_t addr, uintptr_t dest) {
  (void)t;
  if (!addr)
    return -1;
  hook_arm64(addr, dest); // LDR X17,#8 ; BR X17 ; <8-byte target>
  return 0;
}
static int iaml_PlaceRET(This t, uintptr_t addr) {
  (void)t;
  if (!addr)
    return -1;
  *(uint32_t *)addr = 0xd65f03c0u; // RET
  return 0;
}
static bool iaml_Hook(This t, void *addr, void *fn, void **orig) {
  (void)t;
  if (!addr || !fn)
    return false;
  if (orig)
    *orig = addr; // caller can still reach the original prologue directly
  hook_arm64((uintptr_t)addr, (uintptr_t)fn);
  return true;
}

static int iaml_GetAndroidVersion(This t) { (void)t; return 33; /* plausible modern value */ }
static int iaml_GetCPUCores(This t) {
  (void)t;
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}
static bool iaml_IsMainThread(This t) { (void)t; return true; }
static bool iaml_IsGameFaked(This t) { (void)t; return false; }
static bool iaml_HasFastmanAPKModified(This t) { (void)t; return false; }

static bool iaml_FileExists(This t, const char *path) {
  (void)t;
  struct stat st;
  return path && stat(path, &st) == 0;
}
static bool iaml_IsDirectory(This t, const char *path) {
  (void)t;
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static size_t iaml_FileSize(This t, const char *path) {
  (void)t;
  struct stat st;
  if (path && stat(path, &st) == 0)
    return (size_t)st.st_size;
  return 0;
}
static bool iaml_RemoveFile(This t, const char *path) {
  (void)t;
  return path && unlink(path) == 0;
}
static bool iaml_MoveFile(This t, const char *src, const char *dst) {
  (void)t;
  return src && dst && rename(src, dst) == 0;
}
static time_t iaml_GetFileModTime(This t, const char *path) {
  (void)t;
  struct stat st;
  if (path && stat(path, &st) == 0)
    return st.st_mtime;
  return 0;
}

// -- shape-matched stubs (unimplemented, but harmless) ---------------------
//
// AAPCS64: a "return void" stub is safe to install for any void-returning
// slot regardless of the real parameter list, since it never reads its
// arguments. Likewise a stub returning 0 in X0 is safe for any bool/int/
// uintptr_t/size_t/pointer-returning slot (0 == false == NULL). Only the
// float- and std::vector-returning slots need their own exact-shape stub,
// since those use different return registers/ABI.
static void stub_v(This t, ...) { (void)t; }
static uintptr_t stub_0(This t, ...) { (void)t; return 0; }
static float stub_f0(This t, ...) { (void)t; return 0.0f; }
// std::vector<uintptr_t> FindAllPatterns(...): returned via the Itanium
// hidden-return-slot convention (X8 holds the caller's buffer address; we
// must return that same pointer in X0 after leaving the buffer as the
// vector's zeroed/empty state -- 3 null words is a valid empty
// libstdc++/libc++ vector: {begin=null, end=null, cap=null}).
static void *stub_vec0(This t, void *ret_slot, ...) {
  (void)t;
  if (ret_slot)
    memset(ret_slot, 0, sizeof(uintptr_t) * 3);
  return ret_slot;
}

// -- vtable -----------------------------------------------------------------
//
// EXACT order of mod/iaml.h from AndroidModLoader-main (AML 1.4.1), virtuals
// only. Counted and verified as 130 entries; do not reorder.
static void *const iaml_vtable[130] = {
  /* AML 1.0.0.0 */
  (void *)iaml_GetCurrentGame,          //   1
  (void *)iaml_GetConfigPath,           //   2
  (void *)iaml_HasMod,                  //   3
  (void *)iaml_HasModOfVersion,         //   4
  (void *)iaml_GetLib,                  //   5
  (void *)iaml_GetSym_handle,           //   6
  (void *)iaml_Hook,                    //   7
  (void *)stub_0,                       //   8  HookPLT
  (void *)iaml_Unprot,                  //   9
  (void *)iaml_Write,                   //  10
  (void *)iaml_Read,                    //  11
  (void *)iaml_PlaceNOP,                //  12
  (void *)iaml_PlaceJMP,                //  13
  (void *)iaml_PlaceRET,                //  14

  /* AML 1.0.0.4 */
  (void *)iaml_GetDataPath,             //  15

  /* AML 1.0.0.5 */
  (void *)iaml_GetAndroidDataPath,      //  16
  (void *)iaml_GetSym_addr,             //  17

  /* AML 1.0.0.6 */
  (void *)iaml_GetLibLength,            //  18
  (void *)stub_0,                       //  19  Redirect
  (void *)stub_v,                       //  20  PlaceBL
  (void *)stub_v,                       //  21  PlaceBLX
  (void *)iaml_PatternScan_lib,         //  22  PatternScan(pattern, soLib)
  (void *)iaml_PatternScan_range,       //  23  PatternScan(pattern, start, len)

  /* AML 1.0.1 */
  (void *)stub_v,                       //  24  PatchForThumb
  (void *)iaml_GetFeatures,             //  25
  (void *)stub_v,                       //  26  HookVtableFunc (5-arg)
  (void *)iaml_IsGameFaked,             //  27
  (void *)iaml_GetRealCurrentGame,      //  28
  (void *)iaml_GetLibHandle_name,       //  29
  (void *)iaml_GetLibHandle_addr,       //  30
  (void *)stub_0,                       //  31  IsCorrectXDLHandle
  (void *)stub_0,                       //  32  GetLibXDL
  (void *)stub_0,                       //  33  GetAddrBaseXDL
  (void *)stub_0,                       //  34  GetSymSizeXDL
  (void *)stub_0,                       //  35  GetSymNameXDL

  /* AML 1.0.2 */
  (void *)stub_v,                       //  36  ShowToast
  (void *)stub_0,                       //  37  DownloadFile
  (void *)stub_0,                       //  38  DownloadFileToData
  (void *)stub_v,                       //  39  FileMD5
  (void *)stub_0,                       //  40  GetModsLoadedCount
  (void *)stub_0,                       //  41  GetJNIEnvironment
  (void *)stub_0,                       //  42  GetAppContextObject

  /* AML 1.0.2.1 */
  (void *)iaml_HasModOfBiggerVersion,   //  43

  /* AML 1.0.4 */
  (void *)stub_v,                       //  44  HookVtableFunc (6-arg)
  (void *)iaml_PlaceNOP4,               //  45
  (void *)iaml_GetAndroidDataRootPath,  //  46
  (void *)stub_0,                       //  47  HookB
  (void *)stub_0,                       //  48  HookBL
  (void *)stub_0,                       //  49  HookBLX

  /* AML 1.2 */
  (void *)stub_v,                       //  50  MLSSaveFile
  (void *)stub_0,                       //  51  MLSHasValue
  (void *)stub_v,                       //  52  MLSDeleteValue
  (void *)stub_v,                       //  53  MLSSetInt
  (void *)stub_v,                       //  54  MLSSetFloat
  (void *)stub_v,                       //  55  MLSSetInt64
  (void *)stub_v,                       //  56  MLSSetStr
  (void *)stub_0,                       //  57  MLSGetInt
  (void *)stub_0,                       //  58  MLSGetFloat
  (void *)stub_0,                       //  59  MLSGetInt64
  (void *)stub_0,                       //  60  MLSGetStr

  /* AML 1.2.1 */
  (void *)stub_0,                       //  61  IsThumbAddr
  (void *)stub_0,                       //  62  GetBranchDest

  /* AML 1.2.2 */
  (void *)iaml_GetAndroidVersion,       //  63
  (void *)stub_0,                       //  64  CopyFile
  (void *)stub_v,                       //  65  RedirectReg (varargs/32-bit)
  (void *)stub_v,                       //  66  RedirectReg (addr,to,short,reg)
  (void *)stub_0,                       //  67  HasAddrExecFlag
  (void *)stub_v,                       //  68  ToggleHook
  (void *)stub_v,                       //  69  DeHook
  (void *)stub_0,                       //  70  HookInline

  /* AML 1.2.3 */
  (void *)iaml_HasFastmanAPKModified,   //  71
  (void *)iaml_GetInternalPath,         //  72
  (void *)iaml_GetInternalModsPath,     //  73

  /* AML 1.3.0 */
  (void *)stub_0,                       //  74  GetJavaVM
  (void *)stub_0,                       //  75  GetCurrentContext
  (void *)stub_v,                       //  76  DoVibro(int)
  (void *)stub_v,                       //  77  DoVibro(pattern,items)
  (void *)stub_v,                       //  78  CancelVibro
  (void *)stub_f0,                      //  79  GetBatteryLevel

  /* AML 1.4.0 */
  (void *)iaml_GetNativeLibsPath,       //  80
  (void *)stub_0,                       //  81  PushToJavaUIThread
  (void *)stub_0,                       //  82  GetAssetManager
  (void *)stub_0,                       //  83  OpenAsset
  (void *)stub_v,                       //  84  CloseAsset
  (void *)stub_0,                       //  85  GetAssetSize
  (void *)stub_0,                       //  86  GetAssetBuffer
  (void *)stub_v,                       //  87  ReadAsset
  (void *)stub_0,                       //  88  InjectSmaliDEX
  (void *)stub_0,                       //  89  GetInjectedSmaliDEX
  (void *)stub_v,                       //  90  GetDisplaySize
  (void *)stub_0,                       //  91  AllocateMemory
  (void *)stub_0,                       //  92  FreeMemory
  (void *)iaml_ReadPointerChain,        //  93  ReadPointerChain(base, {offsets}) -- initializer_list decomposed as (const int*, size_t) in X2/X3
  (void *)stub_vec0,                    //  94  FindAllPatterns (hidden-return)
  (void *)iaml_ComparePattern,          //  95  ComparePattern
  (void *)stub_v,                       //  96  ShowDialog
  (void *)iaml_FileExists,              //  97
  (void *)iaml_FileSize,                //  98
  (void *)iaml_IsDirectory,             //  99
  (void *)iaml_RemoveFile,              // 100
  (void *)stub_0,                       // 101  RemoveDir
  (void *)stub_0,                       // 102  CreateDirRecursive
  (void *)stub_0,                       // 103  GetCurrentActivity
  (void *)stub_v,                       // 104  GetNewsString
  (void *)stub_0,                       // 105  GetAndroidSystemResID
  (void *)stub_0,                       // 106  ListDir
  (void *)stub_0,                       // 107  ReadFileToBuffer
  (void *)stub_0,                       // 108  WriteBufferToFile
  (void *)iaml_MoveFile,                // 109
  (void *)iaml_GetFileModTime,          // 110
  (void *)stub_v,                       // 111  OpenURL
  (void *)stub_0,                       // 112  CallStaticJavaMethod
  (void *)stub_0,                       // 113  GetStaticJavaField
  (void *)stub_0,                       // 114  SetStaticJavaField
  (void *)stub_0,                       // 115  GetFailedModsCount
  (void *)stub_0,                       // 116  IsFileDownloadsEnabled
  (void *)stub_0,                       // 117  IsMLSInManualSave
  (void *)stub_0,                       // 118  GetDownloadTimeout
  (void *)stub_v,                       // 119  ListMods
  (void *)iaml_IsMainThread,            // 120
  (void *)stub_v,                       // 121  DataMD5
  (void *)stub_0,                       // 122  GetLatestDownloadErrorCode
  (void *)iaml_GetCPUCores,             // 123

  /* AML 1.4.1 */
  (void *)stub_v,                       // 124  ListInterfaces
  (void *)stub_0,                       // 125  GetAppVersionName
  (void *)stub_0,                       // 126  GetAppVersionCode
  (void *)iaml_GetApkPath,              // 127
  (void *)stub_0,                       // 128  GetApkMD5
  (void *)stub_0,                       // 129  GetStringHash
  (void *)stub_0,                       // 130  GetCRC32
};

static const struct { void *const *vtable; } g_iaml_obj = { iaml_vtable };

// -- ICFG ("AMLConfig" interface) -------------------------------------------
//
// mod/config.cpp's Config::Config() does:
//   m_pICFG = (ICFG*)GetInterface("AMLConfig");
//   m_iniMyConfig = m_pICFG->InitIniPointer();     <-- no null-check!
// so if GetInterface("AMLConfig") returns NULL, that second line
// dereferences a null vtable pointer immediately -- this is the crash seen
// after AMLInterface was already working. Real AML backs this with a
// JINI-based .ini parser (src/icfg.cpp); this is a small from-scratch
// equivalent using a fixed-size array of {section,key,value} triples,
// matching real CFG's observable behaviour: GetValueFrom's "unsafe" (5-arg)
// overload NEVER returns NULL, only "" when not found, since callers check
// tryToGetValue[0] without a null-check of their own.
#define ICFG_MAX_ENTRIES 256
typedef struct {
  char section[64];
  char key[64];
  char value[256];
  int used;
} IcfgEntry;
typedef struct {
  IcfgEntry entries[ICFG_MAX_ENTRIES];
  int count;
  char path[384];
} IcfgStore;

static void *icfg_InitIniPointer(This t) {
  (void)t;
  IcfgStore *s = (IcfgStore *)calloc(1, sizeof(IcfgStore));
  return s;
}

static void icfg_ParseInputStream(This t, void *ini, const char *filename) {
  (void)t;
  IcfgStore *s = (IcfgStore *)ini;
  if (!s || !filename)
    return;
  snprintf(s->path, sizeof(s->path), "%s.ini", filename);
  FILE *f = fopen(s->path, "r");
  if (!f)
    return;
  char line[512], section[64] = "";
  while (fgets(line, sizeof(line), f) && s->count < ICFG_MAX_ENTRIES) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    size_t len = strlen(p);
    while (len && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
      p[--len] = 0;
    if (!len || p[0] == ';' || p[0] == '#')
      continue;
    if (p[0] == '[' && p[len - 1] == ']') {
      p[len - 1] = 0;
      strlcpy(section, p + 1, sizeof(section));
      continue;
    }
    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    *eq = 0;
    IcfgEntry *e = &s->entries[s->count++];
    strlcpy(e->section, section, sizeof(e->section));
    strlcpy(e->key, p, sizeof(e->key));
    strlcpy(e->value, eq + 1, sizeof(e->value));
    e->used = 1;
  }
  fclose(f);
}

static void icfg_GenerateToOutputStream(This t, void *ini, const char *filename) {
  (void)t;
  IcfgStore *s = (IcfgStore *)ini;
  if (!s)
    return;
  if (filename)
    snprintf(s->path, sizeof(s->path), "%s.ini", filename);
  if (!s->path[0])
    return;
  FILE *f = fopen(s->path, "w");
  if (!f)
    return;
  char cur_section[64] = "\x01"; // sentinel that can never match a real one
  for (int i = 0; i < s->count; i++) {
    IcfgEntry *e = &s->entries[i];
    if (!e->used)
      continue;
    if (strcmp(cur_section, e->section) != 0) {
      fprintf(f, "[%s]\n", e->section);
      strlcpy(cur_section, e->section, sizeof(cur_section));
    }
    fprintf(f, "%s=%s\n", e->key, e->value);
  }
  fclose(f);
}

static IcfgEntry *icfg_find(IcfgStore *s, const char *section, const char *key) {
  if (!s || !section || !key)
    return NULL;
  for (int i = 0; i < s->count; i++)
    if (s->entries[i].used && strcmp(s->entries[i].section, section) == 0 &&
        strcmp(s->entries[i].key, key) == 0)
      return &s->entries[i];
  return NULL;
}

static const char *icfg_GetValueFrom(This t, void *ini, const char *section, const char *key) {
  (void)t;
  IcfgEntry *e = icfg_find((IcfgStore *)ini, section, key);
  return e ? e->value : ""; // never NULL: config.cpp reads [0] unconditionally
}

static void icfg_SetValueTo(This t, void *ini, const char *section, const char *key, const char *value) {
  (void)t;
  IcfgStore *s = (IcfgStore *)ini;
  if (!s || !section || !key || !value)
    return;
  IcfgEntry *e = icfg_find(s, section, key);
  if (!e && s->count < ICFG_MAX_ENTRIES)
    e = &s->entries[s->count++];
  if (!e)
    return;
  strlcpy(e->section, section, sizeof(e->section));
  strlcpy(e->key, key, sizeof(e->key));
  strlcpy(e->value, value, sizeof(e->value));
  e->used = 1;
}

static bool icfg_GetValueFromSafe(This t, void *ini, const char *section, const char *key, char *out, int maxLen) {
  (void)t;
  IcfgEntry *e = icfg_find((IcfgStore *)ini, section, key);
  if (!e)
    return false;
  strlcpy(out, e->value, (size_t)maxLen);
  return true;
}

static bool icfg_HasSection(This t, void *ini, const char *section) {
  (void)t;
  IcfgStore *s = (IcfgStore *)ini;
  if (!s || !section)
    return false;
  for (int i = 0; i < s->count; i++)
    if (s->entries[i].used && strcmp(s->entries[i].section, section) == 0)
      return true;
  return false;
}
static bool icfg_HasKey(This t, void *ini, const char *section, const char *key) {
  (void)t;
  return icfg_find((IcfgStore *)ini, section, key) != NULL;
}
static bool icfg_HasSectionComment(This t, void *ini, const char *section) {
  (void)t; (void)ini; (void)section;
  return false; // comments aren't tracked by this shim
}
static bool icfg_HasKeyComment(This t, void *ini, const char *section, const char *key) {
  (void)t; (void)ini; (void)section; (void)key;
  return false;
}

// Exact order of mod/icfg.h's ICFG class (10 virtuals).
static void *const icfg_vtable[10] = {
  (void *)icfg_InitIniPointer,
  (void *)icfg_ParseInputStream,
  (void *)icfg_GenerateToOutputStream,
  (void *)icfg_GetValueFrom,
  (void *)icfg_SetValueTo,
  (void *)icfg_GetValueFromSafe,
  (void *)icfg_HasSection,
  (void *)icfg_HasKey,
  (void *)icfg_HasSectionComment,
  (void *)icfg_HasKeyComment,
};
static const struct { void *const *vtable; } g_icfg_obj = { icfg_vtable };

void *aml_get_interface(const char *name) {
  debugPrintf("AML iface: GetInterface(\"%s\")\n", name ? name : "(null)");
  if (name && strcmp(name, "AMLInterface") == 0)
    return (void *)&g_iaml_obj;
  if (name && strcmp(name, "AMLConfig") == 0)
    return (void *)&g_icfg_obj;
  return NULL; // e.g. any per-mod custom interface nobody has registered
}
