// Platform layer on SDL3: window + event thread, gamepads, keyboard.
#include "platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "../host.h"

namespace platform {

namespace {

SDL_Window* g_window = nullptr;
std::atomic<bool> g_resized{false};
bool g_fullscreen = false;

constexpr int kPorts = 4;
std::mutex g_pad_mu;
SDL_Gamepad* g_pads[kPorts] = {};

void ToggleFullscreen() {
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(g_window, g_fullscreen);  // borderless desktop fullscreen
    if (g_fullscreen)
        SDL_HideCursor();
    else
        SDL_ShowCursor();
}

void AddPad(SDL_JoystickID id) {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    for (SDL_Gamepad* p : g_pads)
        if (p && SDL_GetGamepadID(p) == id) return;
    for (int i = 0; i < kPorts; i++)
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            if (g_pads[i]) Log("input: \"%s\" on port %d", SDL_GetGamepadName(g_pads[i]), i);
            return;
        }
}

void RemovePad(SDL_JoystickID id) {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    for (int i = 0; i < kPorts; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            Log("input: port %d disconnected", i);
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = nullptr;
        }
}

void HandleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            Log("window closed");
            std::_Exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            g_resized = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                ToggleFullscreen();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            AddPad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            RemovePad(e.gdevice.which);
            break;
    }
}

void EventThread(int w, int h, bool fullscreen, const char* title, std::mutex* mu, std::condition_variable* cv,
                 bool* done) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) Log("SDL_Init failed: %s", SDL_GetError());
    // Client area at the requested size, shrunk to fit the desktop.
    SDL_Rect work{0, 0, 1920, 1080};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &work);
    float fit = std::min({1.f, 0.9f * work.w / w, 0.9f * work.h / h});
    g_window = SDL_CreateWindow(title, int(w * fit), int(h * fit), SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (!g_window) Log("SDL_CreateWindow failed: %s", SDL_GetError());
    if (g_window && fullscreen) ToggleFullscreen();
    {
        std::lock_guard<std::mutex> lk(*mu);
        *done = true;
    }
    cv->notify_all();
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            HandleEvent(e);
            while (SDL_PollEvent(&e)) HandleEvent(e);
        }
    }
}

}  // namespace

bool CreateMainWindow(const char* title, int w, int h, bool fullscreen) {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::thread(EventThread, w, h, fullscreen, title, &mu, &cv, &done).detach();
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return done; });
    return g_window != nullptr;
}

void* NativeWindowHandle() {
    if (!g_window) return nullptr;
    return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

SDL_Window* Window() { return g_window; }

void DrawableSize(int* w, int* h) {
    *w = *h = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, w, h);
}

bool TakeResize(int* w, int* h) {
    if (!g_resized.exchange(false)) return false;
    DrawableSize(w, h);
    return true;
}

uint32_t ConnectedPads() {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    uint32_t mask = 0;
    for (int i = 0; i < kPorts; i++)
        if (g_pads[i] && SDL_GamepadConnected(g_pads[i])) mask |= 1u << i;
    return mask;
}

bool ReadPad(int port, PadState* out) {
    *out = {};
    std::lock_guard<std::mutex> lk(g_pad_mu);
    SDL_Gamepad* p = port >= 0 && port < kPorts ? g_pads[port] : nullptr;
    if (!p || !SDL_GamepadConnected(p)) return false;
    static const struct { SDL_GamepadButton b; uint16_t bit; } kMap[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadUp},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadLeft},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadRight},
        {SDL_GAMEPAD_BUTTON_START, kPadStart},         {SDL_GAMEPAD_BUTTON_BACK, kPadBack},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadLeftThumb}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadRightThumb},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLeftShoulder}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRightShoulder},
        {SDL_GAMEPAD_BUTTON_SOUTH, kPadA},             {SDL_GAMEPAD_BUTTON_EAST, kPadB},
        {SDL_GAMEPAD_BUTTON_WEST, kPadX},              {SDL_GAMEPAD_BUTTON_NORTH, kPadY},
    };
    for (auto& m : kMap)
        if (SDL_GetGamepadButton(p, m.b)) out->buttons |= m.bit;
    auto trigger = [&](SDL_GamepadAxis a) { return uint8_t(std::max<int>(SDL_GetGamepadAxis(p, a), 0) * 255 / 32767); };
    auto stick = [&](SDL_GamepadAxis a, bool flip) {
        int v = SDL_GetGamepadAxis(p, a);
        return int16_t(flip ? std::clamp(-v, -32768, 32767) : v);
    };
    out->left_trigger = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    out->right_trigger = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    out->lx = stick(SDL_GAMEPAD_AXIS_LEFTX, false), out->ly = stick(SDL_GAMEPAD_AXIS_LEFTY, true);  // SDL: +y is down
    out->rx = stick(SDL_GAMEPAD_AXIS_RIGHTX, false), out->ry = stick(SDL_GAMEPAD_AXIS_RIGHTY, true);
    return true;
}

void Rumble(int port, uint16_t low, uint16_t high) {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    if (port >= 0 && port < kPorts && g_pads[port]) SDL_RumbleGamepad(g_pads[port], low, high, low || high ? 0xFFFFFFFFu : 0);
}

// Arrows = dpad, Z/X/C/V = A/B/X/Y, A/S = left/right shoulder (WHITE/BLACK),
// Q/W = triggers, Enter = START, Backspace = BACK.
void ReadKeyboardPad(PadState* out) {
    *out = {};
    if (!g_window || !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) return;
    const bool* k = SDL_GetKeyboardState(nullptr);
    static const struct { SDL_Scancode sc; uint16_t bit; } kMap[] = {
        {SDL_SCANCODE_UP, kPadUp},     {SDL_SCANCODE_DOWN, kPadDown}, {SDL_SCANCODE_LEFT, kPadLeft},
        {SDL_SCANCODE_RIGHT, kPadRight}, {SDL_SCANCODE_RETURN, kPadStart}, {SDL_SCANCODE_BACKSPACE, kPadBack},
        {SDL_SCANCODE_Z, kPadA},       {SDL_SCANCODE_X, kPadB},       {SDL_SCANCODE_C, kPadX},
        {SDL_SCANCODE_V, kPadY},       {SDL_SCANCODE_A, kPadLeftShoulder}, {SDL_SCANCODE_S, kPadRightShoulder},
    };
    for (auto& m : kMap)
        if (k[m.sc]) out->buttons |= m.bit;
    if (k[SDL_SCANCODE_Q]) out->left_trigger = 255;
    if (k[SDL_SCANCODE_W]) out->right_trigger = 255;
}

}  // namespace platform
