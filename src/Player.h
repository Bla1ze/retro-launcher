#pragma once

#include <string>

// Plays one game on the chosen screen, then execs back to the menu. Only
// returns if that exec fails.
int runPlayer(const std::string& appDir, const std::string& sysId, const std::string& romPath,
              const std::string& screen, int menuIndex, const std::string& returnTo, const std::string& core = "");
