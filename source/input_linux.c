/* Native gamepad input; no keyboard emulation.
 * IDs/axis order are shared by the v2.11.311 per-pad ABI and the older
 * v2.11.264 count-based ABI.
 */
#include "input_linux.h"
#include "jni_fake.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PADS 4
#define BUTTONS 14
#define GN "Java_com_rockstargames_oswrapper_GameNative_"

typedef struct {
  SDL_Gamepad *handle;
  SDL_JoystickID id;
  bool held[BUTTONS];
  float axes[6];
} Gamepad;
static Gamepad pads[MAX_PADS];
static int count;
static bool initialized, focused, trace;
enum InputAbi { INPUT_ABI_COUNT, INPUT_ABI_PER_PAD };
static enum InputAbi input_abi;
static void (*count_changed)(void *, void *, int);
static void (*gamepad_connected)(void *, void *, int);
static void (*gamepad_disconnected)(void *, void *, int);
static void (*gamepad_resume)(void *, void *);
static void (*button_down)(void *, void *, int, int);
static void (*button_up)(void *, void *, int, int);
static void (*axes_changed)(void *, void *, int, float, float, float, float, float, float);

/* SDL logical positions, not Nintendo label-based A/B inversion. */
static const SDL_GamepadButton buttons[BUTTONS] = {
  SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
  SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
  SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
  SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
  SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK
};

/* The Android TV payload uses native slot 15 for physical Minus/Select.
 * Keep this translation separate from SDL's logical button positions.
 */
static const int native_button_ids[BUTTONS] = {
  0, 1, 2, 3, 4, 15, 6, 7, 8, 9, 10, 11, 12, 13
};

static void send_button(int pad, int button, bool down) {
  if (pads[pad].held[button] == down) return;
  pads[pad].held[button] = down;
  (down ? button_down : button_up)(fake_env, NULL, pad,
                                   native_button_ids[button]);
  if (trace) fprintf(stderr, "input: pad=%d button=%d native=%d %s\n",
                     pad, button, native_button_ids[button],
                     down ? "down" : "up");

}

static void check_quit_chord(int pad) {
#ifndef GTASA_QUIT_CHORD
  (void)pad;
  return;
#else
  SDL_Gamepad *gamepad = pads[pad].handle;
  if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START) &&
      (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) ||
       SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE))) {
    jni_quit_requested = 1;
    fprintf(stderr, "input: Back/Guide + Start quit chord\n");
  }
#endif
}

static void send_axes(int pad, const float axes[6], bool force) {
  if (!force && memcmp(pads[pad].axes, axes, sizeof(pads[pad].axes)) == 0) return;
  memcpy(pads[pad].axes, axes, sizeof(pads[pad].axes));
  axes_changed(fake_env, NULL, pad, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
  if (trace) fprintf(stderr, "input: pad=%d axes=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                     pad, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
}

static void reset_pad(int pad) {
  for (int b = 0; b < BUTTONS; b++) send_button(pad, b, false);
  const float zero[6] = {0};
  send_axes(pad, zero, true);
}

static void open_pad(SDL_JoystickID id) {
  for (int i = 0; i < count; i++) if (pads[i].id == id) return;
  if (count == MAX_PADS || !SDL_IsGamepad(id)) return;
  SDL_Gamepad *handle = SDL_OpenGamepad(id);
  if (!handle) {
    fprintf(stderr, "input: cannot open gamepad %u: %s\n", id, SDL_GetError());
    return;
  }
  Gamepad *pad = &pads[count];
  memset(pad, 0, sizeof(*pad));
  pad->handle = handle;
  pad->id = id;
  fprintf(stderr, "input: pad=%d id=%u name=%s\n", count, id, SDL_GetGamepadName(handle));
  char *mapping = SDL_GetGamepadMapping(handle);
  if (mapping) { fprintf(stderr, "input: mapping=%s\n", mapping); SDL_free(mapping); }
  const int slot = count++;
  if (input_abi == INPUT_ABI_PER_PAD)
    gamepad_connected(fake_env, NULL, slot);
  else
    count_changed(fake_env, NULL, count);
  reset_pad(slot);
}

int linux_input_init(so_module *module) {
  if (initialized) linux_input_shutdown();
#define RESOLVE(var, symbol) var = (void *)so_try_find_addr_rx(module, GN symbol)
  RESOLVE(count_changed, "implOnGamepadCountChanged");
  RESOLVE(gamepad_connected, "implOnGamepadConnected");
  RESOLVE(gamepad_disconnected, "implOnGamepadDisconnected");
  RESOLVE(gamepad_resume, "implOnGamepadResume");
  RESOLVE(button_down, "implOnGamepadButtonDown");
  RESOLVE(button_up, "implOnGamepadButtonUp");
  RESOLVE(axes_changed, "implOnGamepadAxesChanged");
#undef RESOLVE
  if (gamepad_connected && gamepad_disconnected) {
    input_abi = INPUT_ABI_PER_PAD;
  } else if (count_changed) {
    input_abi = INPUT_ABI_COUNT;
  } else {
    fprintf(stderr, "input: gamepad JNI interface missing; unsupported game build\n");
    return -1;
  }
  if (!button_down || !button_up || !axes_changed) {
    fprintf(stderr, "input: gamepad button/axis JNI interface missing; unsupported game build\n");
    return -1;
  }
  if (!(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) return -1;
  trace = getenv("GTASA_INPUT_DEBUG") != NULL;
  focused = true;
  initialized = true;
  count = 0;
  if (input_abi == INPUT_ABI_COUNT)
    count_changed(fake_env, NULL, 0);
  int num = 0;
  SDL_JoystickID *ids = SDL_GetJoysticks(&num);
  for (int i = 0; i < num; i++) {
    if (SDL_IsGamepad(ids[i])) open_pad(ids[i]);
    else fprintf(stderr, "input: unmapped joystick %u (%s); supply SDL_GAMECONTROLLERCONFIG\n",
                 ids[i], SDL_GetJoystickNameForID(ids[i]));
  }
  SDL_free(ids);
  fprintf(stderr, "input: %d SDL gamepad(s), native count/buttons/axes API\n", count);
  return 0;
}

void linux_input_event(const SDL_Event *event) {
  if (!initialized) return;
  switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      open_pad(event->gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      for (int p = 0; p < count; p++) {
        if (pads[p].id != event->gdevice.which) continue;
        /* Native slots are contiguous in both supported ABIs. Release every
         * affected slot before compacting so unplug cannot transfer held
         * buttons. The per-pad ABI reconnects shifted slots afterward. */
        for (int i = p; i < count; i++) reset_pad(i);
        if (input_abi == INPUT_ABI_PER_PAD) {
          for (int i = p; i < count; i++)
            gamepad_disconnected(fake_env, NULL, i);
        }
        SDL_CloseGamepad(pads[p].handle);
        memmove(&pads[p], &pads[p + 1], (count - p - 1) * sizeof(pads[0]));
        memset(&pads[--count], 0, sizeof(pads[0]));
        if (input_abi == INPUT_ABI_COUNT) {
          count_changed(fake_env, NULL, count);
        } else {
          for (int i = p; i < count; i++) {
            gamepad_connected(fake_env, NULL, i);
            reset_pad(i);
          }
        }
        fprintf(stderr, "input: removed id=%u; %d gamepad(s)\n", event->gdevice.which, count);
        break;
      }
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      if (!focused) break;
      for (int p = 0; p < count; p++) {
        if (pads[p].id != event->gbutton.which) continue;
        for (int b = 0; b < BUTTONS; b++)
          if (buttons[b] == event->gbutton.button)
            send_button(p, b, event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event->gbutton.button == SDL_GAMEPAD_BUTTON_START ||
             event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK ||
             event->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE))
          check_quit_chord(p);
      }
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      focused = false;
      for (int p = 0; p < count; p++) reset_pad(p);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      focused = true;
      if (input_abi == INPUT_ABI_PER_PAD && gamepad_resume)
        gamepad_resume(fake_env, NULL);
      break;
    default:
      break;
  }
}

void linux_input_update(void) {
  if (!initialized) return;
  /* Some legacy startup stages reset input after Activity/Surface callbacks.
   * The v2.11.264 count callback is idempotent; v2.11.311 uses explicit
   * connected/disconnected callbacks instead. */
  if (input_abi == INPUT_ABI_COUNT)
    count_changed(fake_env, NULL, count);
  if (!focused) return;
  for (int p = 0; p < count; p++) {
    if (!SDL_GamepadConnected(pads[p].handle)) continue;
    for (int b = 0; b < BUTTONS; b++)
      send_button(p, b, SDL_GetGamepadButton(pads[p].handle, buttons[b]));
    float axes[6];
    for (int a = 0; a < 6; a++) {
      Sint16 value = SDL_GetGamepadAxis(pads[p].handle, (SDL_GamepadAxis)a);
      if (a >= 4) axes[a] = value > 0 ? value / 32767.0f : 0.0f;
      else axes[a] = value < 0 ? value / 32768.0f : value / 32767.0f;
    }
    send_axes(p, axes, false);
  }
}

void linux_input_shutdown(void) {
  if (!initialized) return;
  for (int p = 0; p < count; p++) {
    reset_pad(p);
    if (input_abi == INPUT_ABI_PER_PAD)
      gamepad_disconnected(fake_env, NULL, p);
    SDL_CloseGamepad(pads[p].handle);
  }
  memset(pads, 0, sizeof(pads));
  count = 0;
  if (input_abi == INPUT_ABI_COUNT)
    count_changed(fake_env, NULL, 0);
  initialized = false;
}
