// Settings window shown before the game starts (resolution, widescreen,
// fullscreen, renderer). Choices are saved to KakutoChojin.ini.
#pragma once

#include <string>

namespace platform {

// Shows the window when [video] launcher=1 (and KT_NO_LAUNCHER is unset).
// Returns false if the player chose to quit.
bool RunLauncher(const std::string& title);

}  // namespace platform
