#pragma once

#include <string>

// The Neon menu (Systems -> Games). Returns when the user exits the app;
// launching a game replaces the process (see Library::execPlay).
int runMenu(const std::string& appDir, const std::string& sys, int index, const std::string& message);
