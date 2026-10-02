// XAPI input HLE: the Xbox USB/XID stack is replaced by host game controllers
// (SDL gamepads, see platform/). Host controller n is Xbox port n, so up to four players
// are supported. Port 0 is always connected and also reads the keyboard while
// the game window has focus. A watcher thread reflects host plug/unplug in the
// XAPI device-type bitmasks, on which the title's native XGetDeviceChanges
// runs. Rumble is forwarded to the host controller.
#include "../hle.h"
#include "../host.h"
#include "../manifest.h"

#include "../ob.h"
#include "../platform/platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <string>
#include <vector>

namespace d3d {
uint32_t CurrentFrame();
}

namespace {

// XPP_DEVICE_TYPE prefix: connection bitmasks, one bit per port.
struct XppDeviceType {
    ULONG CurrentConnected, ChangeConnected, PreviousConnected;
};

#pragma pack(push, 1)
struct XGamepad {
    WORD wButtons;
    BYTE bAnalogButtons[8];  // A, B, X, Y, BLACK, WHITE, LEFT_TRIGGER, RIGHT_TRIGGER
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XInputStateX {
    DWORD dwPacketNumber;
    XGamepad Gamepad;
};
struct XInputCaps {
    BYTE SubType;
    WORD Reserved;
    XGamepad In;
    BYTE OutRumble[4];
};
#pragma pack(pop)

constexpr int kPorts = 4;
constexpr uint32_t kHandleBase = 0x4B540000;  // handle = base + port + 1 (never null)
constexpr size_t kRumbleOffset = 0x42;         // XINPUT_FEEDBACK: 66-byte header, then XINPUT_RUMBLE
DWORD g_packet[kPorts];
volatile LONG g_connected = 1;                 // port bitmask; port 0 is always present

int PortOf(GHandle h) {
    uint32_t v = h - kHandleBase - 1;
    return v < kPorts ? int(v) : -1;
}

// KT_AUTOPRESS="start@3000-3010,a@4000-4005": scripted presses by frame
// number, for unattended runs and regression tests.
struct ScriptedPress {
    std::string button;
    uint32_t from, to;
};
std::vector<ScriptedPress> LoadScript() {
    std::vector<ScriptedPress> out;
    const char* spec = os::Env("KT_AUTOPRESS");
    if (!spec) return out;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char* tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
        char name[32];
        unsigned a, b;
        if (sscanf(tok, "%31[a-z_]@%u-%u", name, &a, &b) == 3) out.push_back({name, a, b});
    }
    Log("input: %zu scripted press(es)", out.size());
    return out;
}

void ApplyScript(XGamepad* g) {
    static const std::vector<ScriptedPress> script = LoadScript();
    uint32_t frame = d3d::CurrentFrame();
    for (const ScriptedPress& p : script) {
        if (frame < p.from || frame > p.to) continue;
        static const struct { const char* name; WORD digital; int analog; } kButtons[] = {
            {"up", 0x01, -1},    {"down", 0x02, -1},  {"left", 0x04, -1},   {"right", 0x08, -1},
            {"start", 0x10, -1}, {"back", 0x20, -1},  {"a", 0, 0},          {"b", 0, 1},
            {"x", 0, 2},         {"y", 0, 3},         {"black", 0, 4},      {"white", 0, 5},
            {"lt", 0, 6},        {"rt", 0, 7}};
        for (auto& k : kButtons)
            if (p.button == k.name) {
                g->wButtons |= k.digital;
                if (k.analog >= 0) g->bAnalogButtons[k.analog] = 255;
            }
    }
}

XppDeviceType* DeviceType(const char* name) {
    return G2H<XppDeviceType>(HleVar("XAPILIB", name));
}

// Host pad (XInput layout) -> Xbox gamepad (analog face buttons).
void FromHost(const platform::PadState& h, XGamepad* g) {
    g->wButtons |= h.buttons & 0xFF;  // dpad, start, back, thumbs share bits
    const WORD face[6] = {platform::kPadA, platform::kPadB, platform::kPadX, platform::kPadY,
                          platform::kPadRightShoulder /* BLACK */, platform::kPadLeftShoulder /* WHITE */};
    for (int i = 0; i < 6; i++)
        if (h.buttons & face[i]) g->bAnalogButtons[i] = 255;
    g->bAnalogButtons[6] = std::max(g->bAnalogButtons[6], h.left_trigger);
    g->bAnalogButtons[7] = std::max(g->bAnalogButtons[7], h.right_trigger);
    if (h.lx || h.ly) g->sThumbLX = h.lx, g->sThumbLY = h.ly;
    if (h.rx || h.ry) g->sThumbRX = h.rx, g->sThumbRY = h.ry;
}

// Publishes host plug/unplug to XAPI.
void WatchConnections() {
    for (;;) {
        LONG now = LONG(platform::ConnectedPads() | 1);
        LONG before = os::AtomicExchange(&g_connected, now);
        if (now != before) {
            Log("input: ports connected %lx -> %lx", before, now);
            if (XppDeviceType* pad = DeviceType("g_DeviceType_Gamepad")) {
                os::AtomicExchange(&pad->CurrentConnected, ULONG(now));
                os::AtomicOr(&pad->ChangeConnected, ULONG(now ^ before));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void StopRumble() {
    for (int i = 0; i < kPorts; i++) platform::Rumble(i, 0, 0);
}

}  // namespace

VOID WINAPI x_XInitDevices(DWORD, const void*) {
    LONG mask = LONG(platform::ConnectedPads() | 1);
    g_connected = mask;
    if (XppDeviceType* pad = DeviceType("g_DeviceType_Gamepad")) {
        pad->CurrentConnected = mask;
        pad->ChangeConnected = mask;
        pad->PreviousConnected = 0;
    }
    atexit(StopRumble);
    std::thread(WatchConnections).detach();
    Log("XInitDevices: gamepad ports %lx (port 0 = host pad 0 + keyboard)", mask);
}

GHandle WINAPI x_XInputOpen(const void*, DWORD port, DWORD, const void*) {
    // Not connected: a null handle (XAPI's GetLastError reads the title's own
    // thread state, which the host's last-error value never reaches).
    if (port >= kPorts || !(g_connected & (1 << port))) return 0;
    return kHandleBase + port + 1;
}

VOID WINAPI x_XInputClose(GHandle h) {
    int port = PortOf(h);
    if (port >= 0) platform::Rumble(port, 0, 0);
}

DWORD WINAPI x_XInputGetCapabilities(GHandle h, XInputCaps* caps) {
    int port = PortOf(h);
    if (port < 0 || !(g_connected & (1 << port))) return ERROR_DEVICE_NOT_CONNECTED;
    memset(caps, 0xFF, sizeof *caps);
    caps->SubType = 1;  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    caps->Reserved = 0;
    return ERROR_SUCCESS;
}

DWORD WINAPI x_XInputGetState(GHandle h, XInputStateX* state) {
    int port = PortOf(h);
    if (port < 0 || !(g_connected & (1 << port))) return ERROR_DEVICE_NOT_CONNECTED;
    XGamepad g{};
    platform::PadState s;
    bool host = platform::ReadPad(port, &s);
    if (host) FromHost(s, &g);
    else if (port != 0) return ERROR_DEVICE_NOT_CONNECTED;  // unplugged since the last watch
    if (port == 0) {
        platform::ReadKeyboardPad(&s);
        FromHost(s, &g);
        ApplyScript(&g);
    }
    state->dwPacketNumber = ++g_packet[port];
    state->Gamepad = g;
    return ERROR_SUCCESS;
}

// XINPUT_FEEDBACK starts with a header whose dwStatus completes asynchronously
// (the title waits on hEvent or polls dwStatus != ERROR_IO_PENDING).
DWORD WINAPI x_XInputSetState(GHandle h, DWORD* feedback) {
    int port = PortOf(h);
    if (port < 0 || !(g_connected & (1 << port))) return ERROR_DEVICE_NOT_CONNECTED;
    auto* rumble = reinterpret_cast<const WORD*>(reinterpret_cast<const uint8_t*>(feedback) + kRumbleOffset);
    platform::Rumble(port, rumble[0], rumble[1]);
    feedback[0] = ERROR_SUCCESS;
    if (feedback[1]) ob::SetEventHandle(feedback[1]);
    return ERROR_SUCCESS;
}

HLE_EXPORT("XAPILIB", XInitDevices);
HLE_EXPORT("XAPILIB", XInputOpen);
HLE_EXPORT("XAPILIB", XInputClose);
HLE_EXPORT("XAPILIB", XInputGetCapabilities);
HLE_EXPORT("XAPILIB", XInputGetState);
HLE_EXPORT("XAPILIB", XInputSetState);
