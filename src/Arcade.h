#pragma once

// Arcade sets: which emulator can run each zip in roms/arcade/, and what the
// game is called. Each arcade core ships a ROM database next to it
// (cores/<core>.db, made by tools/make_arcade_db.py from the same source the
// core was built from). A zip's own index lists the CRC of every ROM inside it,
// so a set is checked against each database without unpacking anything:
// complete for FBNeo -> FBNeo; only for MAME 2003-Plus -> MAME; neither -> the
// closest, with the reason shown.

#include "Library.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Arcade {

// One game in a core's database.
struct Entry {
    std::string name, parent, romof, flags, year, maker, title;
    std::vector<uint32_t> crcs;  // every ROM it needs (own, parent's, BIOS'), no-dumps left out
    bool vertical() const { return flags.find('V') != std::string::npos; }
    bool bios() const { return flags.find('B') != std::string::npos; }
};

// "FBNeo" / "MAME 2003-Plus" for a core file name.
std::string coreLabel(const std::string& coreFile);

// Scans roms/arcade/ (zip indexes cached in data/arcade-zips.txt) and fills in
// each game's title, details, emulator choice, orientation and any problem.
// BIOS zips (neogeo.zip...) are left out of the list.
std::vector<Library::Game> scan(const std::string& appDir, const Library::System& sys);

// One game's entry in a core's database, without loading all of it (for the
// player, which only needs the title).
bool lookup(const std::string& appDir, const std::string& coreFile, const std::string& name, Entry& out);

} // namespace Arcade
