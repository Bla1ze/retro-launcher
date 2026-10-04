#include "Arcade.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace Arcade {

using Library::log;

namespace {

std::string lowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
        size_t b = line.find('\t', a);
        f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return f;
        a = b + 1;
    }
}

bool parseEntry(const std::string& line, Entry& e) {
    if (line.empty() || line[0] == '#') return false;
    std::vector<std::string> f = splitTabs(line);
    if (f.size() < 8) return false;
    e.name = f[0]; e.parent = f[1]; e.romof = f[2]; e.flags = f[3];
    e.year = f[4]; e.maker = f[5]; e.title = f[6];
    e.crcs.clear();
    const char* p = f[7].c_str();
    while (*p) {
        char* end = nullptr;
        unsigned long v = std::strtoul(p, &end, 16);
        if (end == p) break;
        e.crcs.push_back((uint32_t)v);
        p = end;
        while (*p == ' ') ++p;
    }
    return true;
}

using Db = std::unordered_map<std::string, Entry>;

std::string dbPath(const std::string& appDir, const std::string& coreFile) {
    std::string base = coreFile.substr(0, coreFile.find_last_of('.'));
    return appDir + "/cores/" + base + ".db";
}

bool loadDb(const std::string& path, Db& db) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    Entry e;
    while (std::getline(in, line))
        if (parseEntry(line, e)) db[e.name] = e;
    return true;
}

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// The CRC of every file in a zip, from its central directory only.
bool zipCrcs(const std::string& path, std::vector<uint32_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    long tail = std::min(size, 65536L + 22);
    std::vector<uint8_t> buf((size_t)tail);
    std::fseek(f, size - tail, SEEK_SET);
    bool ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    long eocd = -1;
    for (long i = tail - 22; ok && i >= 0; --i)
        if (rd32(&buf[(size_t)i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { std::fclose(f); return false; }
    uint32_t cdSize = rd32(&buf[(size_t)eocd + 12]), cdOff = rd32(&buf[(size_t)eocd + 16]);
    if ((long)cdOff + (long)cdSize > size) { std::fclose(f); return false; }
    std::vector<uint8_t> cd(cdSize);
    std::fseek(f, cdOff, SEEK_SET);
    ok = std::fread(cd.data(), 1, cd.size(), f) == cd.size();
    std::fclose(f);
    if (!ok) return false;
    out.clear();
    for (size_t p = 0; p + 46 <= cd.size() && rd32(&cd[p]) == 0x02014b50;) {
        if (rd32(&cd[p + 24]) > 0) out.push_back(rd32(&cd[p + 16]));  // skip folders / empty files
        p += 46 + rd16(&cd[p + 28]) + rd16(&cd[p + 30]) + rd16(&cd[p + 32]);
    }
    return true;
}

// data/arcade-zips.txt: "<file>\t<size>\t<mtime>\t<crc crc ...>" so a rescan only
// opens zips that are new or changed.
struct ZipInfo { long long size = 0, mtime = 0; std::vector<uint32_t> crcs; };

std::map<std::string, ZipInfo> loadZipCache(const std::string& path) {
    std::map<std::string, ZipInfo> cache;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f = splitTabs(line);
        if (f.size() < 4) continue;
        ZipInfo z;
        z.size = std::atoll(f[1].c_str());
        z.mtime = std::atoll(f[2].c_str());
        std::istringstream crcs(f[3]);
        std::string c;
        while (crcs >> c) z.crcs.push_back((uint32_t)std::strtoul(c.c_str(), nullptr, 16));
        cache[f[0]] = z;
    }
    return cache;
}

void saveZipCache(const std::string& path, const std::map<std::string, ZipInfo>& cache) {
    std::ofstream out(path);
    char hex[12];
    for (const auto& kv : cache) {
        out << kv.first << '\t' << kv.second.size << '\t' << kv.second.mtime << '\t';
        for (size_t i = 0; i < kv.second.crcs.size(); ++i) {
            std::snprintf(hex, sizeof(hex), "%s%08x", i ? " " : "", kv.second.crcs[i]);
            out << hex;
        }
        out << '\n';
    }
}

// How well one core can run one set, given the zips in the folder.
struct Verdict {
    bool known = false, complete = false;
    size_t missing = 0;
    std::string problem;  // "" when complete
};

Verdict judge(const Db& db, const std::string& name, const std::map<std::string, ZipInfo>& zips) {
    Verdict v;
    auto it = db.find(name);
    if (it == db.end()) return v;
    v.known = true;
    const Entry& e = it->second;
    if (e.flags.find('C') != std::string::npos) { v.problem = "Needs a CHD disk image (not supported)"; v.missing = 1u << 20; return v; }
    // What the folder offers: this zip, its parent's, and the parent's parent.
    std::set<uint32_t> have;
    auto add = [&](const std::string& n) {
        auto z = zips.find(n);
        if (z != zips.end()) have.insert(z->second.crcs.begin(), z->second.crcs.end());
        return z != zips.end();
    };
    add(name);
    bool parentThere = e.parent.empty() || add(e.parent);
    std::string grand;
    if (!e.parent.empty()) {
        auto p = db.find(e.parent);
        if (p != db.end() && !p->second.parent.empty()) { grand = p->second.parent; add(grand); }
    }
    // A BIOS (romof pointing at a BIOS set, directly or through the parent):
    // its zip has to be there, but FBNeo lists every alternative BIOS chip, which
    // few BIOS zips have all of, so its ROMs only need to be present in part.
    std::string biosName;
    for (const std::string* cand : {&e.romof, &e.parent}) {
        auto b = db.find(*cand);
        if (b != db.end() && b->second.bios()) { biosName = *cand; break; }
        if (b != db.end() && !b->second.romof.empty()) {
            auto bb = db.find(b->second.romof);
            if (bb != db.end() && bb->second.bios()) { biosName = b->second.romof; break; }
        }
    }
    std::set<uint32_t> biosCrcs;
    bool biosThere = true;
    if (!biosName.empty()) {
        biosCrcs.insert(db.at(biosName).crcs.begin(), db.at(biosName).crcs.end());
        biosThere = add(biosName);
    }
    for (uint32_t c : e.crcs)
        if (!biosCrcs.count(c) && !have.count(c)) ++v.missing;
    if (!biosThere) { v.problem = "Needs " + biosName + ".zip"; v.missing += 1000; return v; }
    if (v.missing && !parentThere) { v.problem = "Needs " + e.parent + ".zip"; return v; }
    if (v.missing) { v.problem = std::to_string(v.missing) + " ROM" + (v.missing == 1 ? "" : "s") + " missing or wrong version"; return v; }
    v.complete = true;
    return v;
}

} // namespace

std::string coreLabel(const std::string& coreFile) {
    if (coreFile.compare(0, 5, "fbneo") == 0) return "FBNeo";
    if (coreFile.compare(0, 13, "mame2003_plus") == 0) return "MAME 2003-Plus";
    if (coreFile.compare(0, 8, "mame2010") == 0) return "MAME 2010";
    return coreFile;
}

std::vector<Library::Game> scan(const std::string& appDir, const Library::System& sys) {
    std::vector<Library::Game> games;
    const std::string dir = appDir + "/roms/" + sys.id;
    // The zips, with their indexes (cached).
    const std::string cachePath = appDir + "/data/arcade-zips.txt";
    std::map<std::string, ZipInfo> cache = loadZipCache(cachePath), zips;  // zips: by set name
    std::map<std::string, std::string> fileOf;
    bool changed = false;
    int opened = 0;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (struct dirent* de = ::readdir(d)) {
            std::string file = de->d_name;
            if (file.size() < 5 || file[0] == '.' || lowerStr(file.substr(file.size() - 4)) != ".zip") continue;
            struct stat st;
            if (::stat((dir + "/" + file).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            ZipInfo& z = cache[file];
            if (z.size != (long long)st.st_size || z.mtime != (long long)st.st_mtime || z.crcs.empty()) {
                z.size = st.st_size;
                z.mtime = st.st_mtime;
                if (!zipCrcs(dir + "/" + file, z.crcs)) log("arcade: could not read %s", file.c_str());
                changed = true;
                ++opened;
            }
            std::string set = lowerStr(file.substr(0, file.size() - 4));
            zips[set] = z;
            fileOf[set] = file;
        }
        ::closedir(d);
    }
    if (zips.empty()) return games;
    if (changed) {
        for (auto it = cache.begin(); it != cache.end();)  // forget deleted zips
            it = zips.count(lowerStr(it->first.substr(0, it->first.size() - 4))) ? std::next(it) : cache.erase(it);
        saveZipCache(cachePath, cache);
    }
    // Each core's database, in preference order.
    std::vector<std::pair<std::string, Db>> dbs;
    for (const std::string& core : sys.cores) {
        Db db;
        if (loadDb(dbPath(appDir, core), db)) dbs.emplace_back(core, std::move(db));
    }
    log("arcade: %zu zip(s), %d read, %zu database(s)", zips.size(), opened, dbs.size());

    int complete = 0;
    for (const auto& kv : zips) {
        const std::string& set = kv.first;
        bool isBios = false;
        for (const auto& db : dbs) {
            auto e = db.second.find(set);
            if (e != db.second.end() && e->second.bios()) isBios = true;
        }
        if (isBios) continue;
        Library::Game g;
        g.file = fileOf[set];
        g.path = dir + "/" + g.file;
        g.title = set;
        g.arcade = true;
        const Entry* named = nullptr;
        std::string bestCore, bestProblem = "Not a set these emulators know";
        size_t bestMissing = (size_t)-1;
        for (const auto& db : dbs) {
            Verdict v = judge(db.second, set, zips);
            if (!v.known) continue;
            const Entry& e = db.second.at(set);
            if (!named) named = &e;
            if (v.complete) {
                g.cores.push_back(db.first);
                if (bestCore.empty() || bestMissing > 0) { bestCore = db.first; bestMissing = 0; bestProblem.clear(); named = &e; }
            } else if (bestMissing > 0 && v.missing < bestMissing) {
                bestCore = db.first; bestMissing = v.missing; bestProblem = v.problem; named = &e;
            }
        }
        if (g.cores.empty() && !bestCore.empty()) g.cores.push_back(bestCore);
        g.core = bestCore;
        g.problem = bestProblem;
        if (named) {
            g.title = named->title;
            g.vertical = named->vertical();
            g.tags = named->year + (named->maker.empty() ? "" : "  " + named->maker);
            if (named->flags.find('P') != std::string::npos) g.tags += "  (not working)";
        }
        if (!g.problem.empty()) g.tags = g.problem + (g.tags.empty() ? "" : "  -  " + g.tags);
        else ++complete;
        games.push_back(g);
    }
    std::sort(games.begin(), games.end(),
              [](const Library::Game& a, const Library::Game& b) { return lowerStr(a.title) < lowerStr(b.title); });
    log("arcade: %zu game(s), %d ready", games.size(), complete);
    return games;
}

bool lookup(const std::string& appDir, const std::string& coreFile, const std::string& name, Entry& out) {
    std::ifstream in(dbPath(appDir, coreFile));
    std::string line, prefix = name + "\t";
    while (std::getline(in, line))
        if (line.compare(0, prefix.size(), prefix) == 0) return parseEntry(line, out);
    return false;
}

} // namespace Arcade
