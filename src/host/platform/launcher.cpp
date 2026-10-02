// Pre-game settings window on SDL3 (renderer + built-in debug font, so no
// UI library is needed). Keyboard, mouse and game controllers work:
//   Up/Down choose a row, Left/Right change it, Enter/A starts, Esc/B quits.
#include "launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../os.h"

namespace platform {

namespace {

struct Choice {
    const char* label;
    std::string value;  // as stored in KakutoChojin.ini
};

struct Row {
    const char* name;
    const char* key;  // [video] key; null for the buttons
    std::vector<Choice> choices;
    int current = 0;
};

int IndexOf(const Row& r, const std::string& v) {
    for (size_t i = 0; i < r.choices.size(); i++)
        if (r.choices[i].value == v) return int(i);
    return 0;
}

}  // namespace

bool RunLauncher(const std::string& title) {
    if (os::EnvSet("KT_NO_LAUNCHER") || os::IniInt("video", "launcher", 0) == 0) return true;

    std::vector<Row> rows = {
        {"Resolution", "resolution",
         {{"Original (480p)", "0"}, {"HD (720p)", "720"}, {"Full HD (1080p)", "1080"}, {"WQHD (1440p)", "1440"},
          {"4K (2160p)", "2160"}}},
        {"Aspect ratio", "widescreen", {{"4:3 (original)", "0"}, {"16:9 widescreen", "1"}}},
        {"Display", "fullscreen", {{"Window", "0"}, {"Fullscreen", "1"}}},
        {"Renderer", "gpu", {{"Direct3D 11", "d3d11"}, {"OpenGL 4.5", "gl"}}},
        {"Show this window", "launcher", {{"At every start", "1"}, {"Not again (KakutoChojin.ini)", "0"}}},
        {"Start game", nullptr, {}},
        {"Quit", nullptr, {}},
    };
#ifndef _WIN32
    rows[3].choices.erase(rows[3].choices.begin());  // OpenGL only
#endif
    for (Row& r : rows)
        if (r.key) r.current = IndexOf(r, os::IniString("video", r.key, r.key == std::string("gpu") ? "d3d11" : r.choices.front().value));
    const int start_row = 5, quit_row = 6;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return true;  // no UI possible: just start
    constexpr int kScale = 2, kW = 880, kH = 420;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    std::string caption = title.empty() ? "Settings" : title + " - Settings";
    if (!SDL_CreateWindowAndRenderer(caption.c_str(), kW, kH, 0, &win, &ren)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return true;
    }
    SDL_SetRenderVSync(ren, 1);
    std::vector<SDL_Gamepad*> pads;

    constexpr float kRowY = 96, kRowH = 34, kValueX = 300;
    int sel = start_row;
    bool done = false, start = true;
    auto change = [&](int dir) {
        Row& r = rows[sel];
        if (r.choices.empty()) return;
        int n = int(r.choices.size());
        r.current = (r.current + dir + n) % n;
    };
    auto activate = [&] {
        if (sel == start_row) done = true, start = true;
        else if (sel == quit_row) done = true, start = false;
        else change(1);
    };
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    done = true, start = false;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (SDL_Gamepad* p = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(p);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    switch (e.key.key) {
                        case SDLK_UP: sel = (sel + int(rows.size()) - 1) % int(rows.size()); break;
                        case SDLK_DOWN: sel = (sel + 1) % int(rows.size()); break;
                        case SDLK_LEFT: change(-1); break;
                        case SDLK_RIGHT: change(1); break;
                        case SDLK_RETURN: case SDLK_SPACE: activate(); break;
                        case SDLK_ESCAPE: done = true, start = false; break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    switch (e.gbutton.button) {
                        case SDL_GAMEPAD_BUTTON_DPAD_UP: sel = (sel + int(rows.size()) - 1) % int(rows.size()); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: sel = (sel + 1) % int(rows.size()); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: change(-1); break;
                        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: change(1); break;
                        case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: activate(); break;
                        case SDL_GAMEPAD_BUTTON_EAST: done = true, start = false; break;
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    float y = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
                    int r = int((y - kRowY + 6) / kRowH);
                    if (r >= 0 && r < int(rows.size())) {
                        sel = r;
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                            if (e.button.button == SDL_BUTTON_RIGHT) change(-1);
                            else activate();
                        }
                    }
                    break;
                }
            }
        }
        SDL_SetRenderScale(ren, 1, 1);
        SDL_SetRenderDrawColor(ren, 24, 26, 32, 255);
        SDL_RenderClear(ren);
        SDL_SetRenderDrawColor(ren, 60, 90, 160, 255);
        SDL_FRect bar{0, kRowY + sel * kRowH - 6, float(kW), kRowH - 4};
        SDL_RenderFillRect(ren, &bar);
        SDL_SetRenderScale(ren, kScale, kScale);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderDebugText(ren, 12, 10, (title.empty() ? std::string("Settings") : title).substr(0, 48).c_str());
        SDL_SetRenderDrawColor(ren, 150, 150, 160, 255);
        SDL_RenderDebugText(ren, 12, 22, "Up/Down: choose  Left/Right: change");
        for (size_t i = 0; i < rows.size(); i++) {
            float y = (kRowY + i * kRowH) / kScale;
            const Row& r = rows[i];
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            SDL_RenderDebugText(ren, 12, y, r.name);
            if (!r.choices.empty()) {
                std::string v = std::string("< ") + r.choices[r.current].label + " >";
                SDL_SetRenderDrawColor(ren, 255, 220, 120, 255);
                SDL_RenderDebugText(ren, kValueX / kScale, y, v.c_str());
            }
        }
        SDL_SetRenderDrawColor(ren, 150, 150, 160, 255);
        SDL_RenderDebugText(ren, 12, (kH - 22) / kScale, "Enter/A: start  Esc/B: quit  F11: fullscreen");
        // KT_LAUNCHER_SHOT=<file.bmp>: save this frame and start (unattended tests).
        if (const char* shot = os::Env("KT_LAUNCHER_SHOT")) {
            if (SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr)) SDL_SaveBMP(s, shot), SDL_DestroySurface(s);
            done = true, start = true;
        }
        SDL_RenderPresent(ren);
    }

    if (start)
        for (const Row& r : rows)
            if (r.key) os::IniSet("video", r.key, r.choices[r.current].value);
    for (SDL_Gamepad* p : pads) SDL_CloseGamepad(p);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    return start;
}

}  // namespace platform
