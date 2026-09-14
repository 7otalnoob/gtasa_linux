#include "input_linux.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void *fake_env;
volatile int jni_quit_requested;
static int native_count, down_calls, up_calls, resume_calls;
static bool connected_slots[4];
static bool held[4][16];
static float axes[4][6];
static bool missing;
static const int expected_native_buttons[14] = {
  0, 1, 2, 3, 4, 15, 6, 7, 8, 9, 10, 11, 12, 13
};


static void set_count(void *env, void *obj, int n) {
  (void)env; (void)obj;
  assert(n >= 0 && n <= 4);
  native_count = n;
}
static void connected(void *env, void *obj, int p) {
  (void)env; (void)obj;
  assert(p >= 0 && p < 4 && !connected_slots[p]);
  connected_slots[p] = true;
  native_count++;
}
static void disconnected(void *env, void *obj, int p) {
  (void)env; (void)obj;
  assert(p >= 0 && p < 4 && connected_slots[p]);
  connected_slots[p] = false;
  native_count--;
}
static void resume(void *env, void *obj) {
  (void)env; (void)obj;
  resume_calls++;
}
static void down(void *env, void *obj, int p, int b) {
  (void)env; (void)obj;
  assert(p >= 0 && p < native_count && b >= 0 && b < 16);
  assert(!held[p][b]);
  held[p][b] = true;
  down_calls++;
}
static void up(void *env, void *obj, int p, int b) {
  (void)env; (void)obj;
  assert(p >= 0 && p < native_count && b >= 0 && b < 16);
  assert(held[p][b]);
  held[p][b] = false;
  up_calls++;
}
static void axis(void *env, void *obj, int p, float lx, float ly,
                 float rx, float ry, float lt, float rt) {
  (void)env; (void)obj;
  assert(p >= 0 && p < native_count);
  float values[] = {lx, ly, rx, ry, lt, rt};
  memcpy(axes[p], values, sizeof(values));
}
uintptr_t so_try_find_addr_rx(so_module *module, const char *name) {
  (void)module;
  if (missing) return 0;
  if (strstr(name, "GamepadConnected")) return (uintptr_t)connected;
  if (strstr(name, "GamepadDisconnected")) return (uintptr_t)disconnected;
  if (strstr(name, "GamepadResume")) return (uintptr_t)resume;
  if (strstr(name, "CountChanged")) return (uintptr_t)set_count;
  if (strstr(name, "ButtonDown")) return (uintptr_t)down;
  if (strstr(name, "ButtonUp")) return (uintptr_t)up;
  if (strstr(name, "AxesChanged")) return (uintptr_t)axis;
  return 0;
}
static void pump(void) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) linux_input_event(&event);
  linux_input_update();
}
static SDL_JoystickID attach(void) {
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
  desc.name = "GTA SDL virtual regression pad";
  SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  assert(id && SDL_IsGamepad(id));
  return id;
}
int main(void) {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  assert(SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS));
  SDL_JoystickID first = attach();
  SDL_Joystick *joystick = SDL_OpenJoystick(first);
  assert(joystick);
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768));
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768));
  assert(linux_input_init(NULL) == 0);
  jni_quit_requested = 0;
  pump();
  assert(native_count == 1);
  const SDL_GamepadButton map[] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK
  };
  for (int b = 0; b < 14; b++) {
    assert(SDL_SetJoystickVirtualButton(joystick, map[b], true));
    pump(); assert(held[0][expected_native_buttons[b]]);
    assert(SDL_SetJoystickVirtualButton(joystick, map[b], false));
    pump(); assert(!held[0][expected_native_buttons[b]]);
  }
  assert(down_calls == 14 && up_calls == 14);
  jni_quit_requested = 0;
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_BACK, true));
  pump();
  assert(!jni_quit_requested);
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, true));
  pump();
#ifdef GTASA_QUIT_CHORD
  assert(jni_quit_requested);
#else
  assert(!jni_quit_requested);
#endif
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, false));
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_BACK, false));
  pump();
  jni_quit_requested = 0;
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_GUIDE, true));
  pump();
  assert(!jni_quit_requested);
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, true));
  pump();
#ifdef GTASA_QUIT_CHORD
  assert(jni_quit_requested);
#else
  assert(!jni_quit_requested);
#endif
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, false));
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_GUIDE, false));
  pump();
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, -32768));
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, 32767));
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 16384));
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, -16384));
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768));
  assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767));
  pump();
  assert(axes[0][0] == -1 && axes[0][1] == 1);
  assert(fabsf(axes[0][2] - 0.5f) < 0.001f && axes[0][3] == -0.5f);
  assert(axes[0][4] == 0 && axes[0][5] == 1);
  assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
  pump();
  SDL_Event event = { .type = SDL_EVENT_WINDOW_FOCUS_LOST };
  linux_input_event(&event); pump();
  assert(!held[0][0] && axes[0][0] == 0 && axes[0][5] == 0);
  event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  linux_input_event(&event); pump();
  assert(held[0][0] && axes[0][0] == -1);
  assert(resume_calls == 1);
  SDL_JoystickID second = attach();
  SDL_Joystick *other = SDL_OpenJoystick(second);
  assert(other);
  pump(); assert(native_count == 2);
  assert(SDL_SetJoystickVirtualButton(other, SDL_GAMEPAD_BUTTON_EAST, true));
  pump(); assert(held[1][1]);
  assert(SDL_DetachVirtualJoystick(first));
  pump(); assert(native_count == 1 && !held[0][0] && held[0][1] && !held[1][1]);
  assert(SDL_DetachVirtualJoystick(second));
  pump(); assert(native_count == 0 && !held[0][1]);
  SDL_CloseJoystick(joystick);
  SDL_CloseJoystick(other);
  linux_input_shutdown();
  linux_input_shutdown();
  missing = true;
  assert(linux_input_init(NULL) == -1);
  linux_input_shutdown();
  SDL_Quit();
  puts("input regression: PASS (v2.11.311 per-pad ABI, 14 buttons, six axes, focus reset, startup/hotplug/compaction, missing ABI)");
  return 0;
}
