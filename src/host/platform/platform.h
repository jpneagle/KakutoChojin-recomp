// Host platform layer: window, events and game controllers (SDL3).
//
// The window and its event loop live on their own thread, so title threads
// never pump events. Graphics backends get the native window from here.
#pragma once

#include <cstdint>

struct SDL_Window;

namespace platform {

// Creates the window (client area w x h, shrunk to fit the desktop) and
// starts the event thread. Returns false if no window could be created.
bool CreateMainWindow(const char* title, int w, int h, bool fullscreen);

void* NativeWindowHandle();   // HWND on Windows, null elsewhere
SDL_Window* Window();  // the SDL window (for GL contexts)

// Current drawable size in pixels; returns true once after each resize.
bool TakeResize(int* w, int* h);
void DrawableSize(int* w, int* h);

// Controller state in XInput layout.
struct PadState {
    uint16_t buttons;  // XINPUT_GAMEPAD_* bits (dpad, start, back, thumbs, shoulders, ABXY)
    uint8_t left_trigger, right_trigger;
    int16_t lx, ly, rx, ry;
};
enum : uint16_t {
    kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008,
    kPadStart = 0x0010, kPadBack = 0x0020, kPadLeftThumb = 0x0040, kPadRightThumb = 0x0080,
    kPadLeftShoulder = 0x0100, kPadRightShoulder = 0x0200,
    kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000,
};

// Host controllers fill ports 0-3 in connection order.
uint32_t ConnectedPads();  // bitmask
bool ReadPad(int port, PadState* out);
void Rumble(int port, uint16_t low, uint16_t high);

// Keyboard mapped onto a pad (only while the window has focus).
void ReadKeyboardPad(PadState* out);

}  // namespace platform
