#include "ArtDownload.h"

#include "GamePanels.h"
#include "Library.h"
#include "Net.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using Library::log;
using Net::isNetworkError;
using Net::jsonNext;
using Net::jsonString;
using Net::readFile;
using Net::urlEncode;

namespace {

#ifndef ART_LIBRETRO_HOST
#define ART_LIBRETRO_HOST "https://thumbnails.libretro.com"  // a harness can point it elsewhere
#endif
const char* kUserAgent = "retro-launcher (https://github.com/Bla1ze/retro-launcher)";
const long kListingMaxAge = 7 * 24 * 3600;  // a week, like tools/prefill_boxart.py
const int kParallel = 4;
const int kBroken = -3;  // fetch's rc for a file that downloaded but is incomplete at its source


// libretro-thumbnails sets per system (as tools/prefill_boxart.py). Logos for
// NAOMI and Atomiswave are MAME's and go in media/arcade/, where findLogo looks.
struct Src {
    const char* sys;
    std::vector<const char*> covers, logos;
    const char* logoSys;
};
const std::vector<Src>& sources() {
    static const std::vector<Src> list = {
        {"arcade", {"FBNeo_-_Arcade_Games", "MAME"}, {"MAME"}, "arcade"},
        {"naomi", {"Sega_-_Naomi", "Sega_-_Naomi_2"}, {"MAME"}, "arcade"},
        {"atomiswave", {"Atomiswave"}, {"MAME"}, "arcade"},
        {"neogeo", {"SNK_-_Neo_Geo", "FBNeo_-_Arcade_Games"}, {"MAME"}, "arcade"},
        {"genesis", {"Sega_-_Mega_Drive_-_Genesis"}, {"Sega_-_Mega_Drive_-_Genesis"}, "genesis"},
        {"mastersystem", {"Sega_-_Master_System_-_Mark_III"}, {"Sega_-_Master_System_-_Mark_III"}, "mastersystem"},
        {"gamegear", {"Sega_-_Game_Gear"}, {"Sega_-_Game_Gear"}, "gamegear"},
        {"nes", {"Nintendo_-_Nintendo_Entertainment_System"}, {"Nintendo_-_Nintendo_Entertainment_System"}, "nes"},
        {"snes", {"Nintendo_-_Super_Nintendo_Entertainment_System"}, {"Nintendo_-_Super_Nintendo_Entertainment_System"}, "snes"},
        {"atari2600", {"Atari_-_2600"}, {"Atari_-_2600"}, "atari2600"},
        {"colecovision", {"Coleco_-_ColecoVision"}, {"Coleco_-_ColecoVision"}, "colecovision"},
        {"gb", {"Nintendo_-_Game_Boy"}, {"Nintendo_-_Game_Boy"}, "gb"},
        {"gbc", {"Nintendo_-_Game_Boy_Color"}, {"Nintendo_-_Game_Boy_Color"}, "gbc"},
        {"gba", {"Nintendo_-_Game_Boy_Advance"}, {"Nintendo_-_Game_Boy_Advance"}, "gba"},
        {"pce", {"NEC_-_PC_Engine_-_TurboGrafx_16", "NEC_-_PC_Engine_SuperGrafx"},
         {"NEC_-_PC_Engine_-_TurboGrafx_16", "NEC_-_PC_Engine_SuperGrafx"}, "pce"},
        {"lynx", {"Atari_-_Lynx"}, {"Atari_-_Lynx"}, "lynx"},
        {"psx", {"Sony_-_PlayStation"}, {"Sony_-_PlayStation"}, "psx"},
        {"dreamcast", {"Sega_-_Dreamcast"}, {"Sega_-_Dreamcast"}, "dreamcast"},
        {"psp", {"Sony_-_PlayStation_Portable"}, {"Sony_-_PlayStation_Portable"}, "psp"},
        {"n64", {"Nintendo_-_Nintendo_64"}, {"Nintendo_-_Nintendo_64"}, "n64"},
        {"saturn", {"Sega_-_Saturn"}, {"Sega_-_Saturn"}, "saturn"},
    };
    return list;
}

// The Bezel Project's set per system (as tools/fetch_media.sh); PSP has none.
const char* bezelRepo(const std::string& id) {
    static const std::pair<const char*, const char*> repos[] = {
        {"genesis", "bezelproject-MegaDrive"}, {"mastersystem", "bezelproject-MasterSystem"},
        {"gamegear", "bezelproject-GameGear"}, {"nes", "bezelproject-NES"}, {"snes", "bezelproject-SNES"},
        {"atari2600", "bezelproject-Atari2600"}, {"colecovision", "bezelproject-ColecoVision"},
        {"gb", "bezelproject-GB"}, {"gbc", "bezelproject-GBC"}, {"gba", "bezelproject-GBA"},
        {"pce", "bezelproject-PCEngine"}, {"lynx", "bezelproject-AtariLynx"}, {"psx", "bezelproject-PSX"},
        {"dreamcast", "bezelproject-Dreamcast"}, {"n64", "bezelproject-N64"}, {"saturn", "bezelproject-Saturn"},
        {"naomi", "bezelproject-Naomi"}, {"atomiswave", "bezelproject-Atomiswave"}};
    for (const auto& r : repos)
        if (id == r.first) return r.second;
    return nullptr;
}

bool isFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}



std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string htmlDecode(std::string s) {
    static const std::pair<const char*, const char*> ents[] = {
        {"&quot;", "\""}, {"&#39;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (const auto& e : ents)
        for (size_t at = s.find(e.first); at != std::string::npos; at = s.find(e.first, at + 1))
            s.replace(at, std::strlen(e.first), e.second);
    return s;
}

// The .png links of an Apache directory listing.
std::vector<std::string> parseListing(const std::string& html) {
    std::vector<std::string> names;
    for (size_t at = html.find("href=\""); at != std::string::npos; at = html.find("href=\"", at)) {
        at += 6;
        size_t end = html.find('"', at);
        if (end == std::string::npos) break;
        std::string n = urlDecode(htmlDecode(html.substr(at, end - at)));
        at = end;
        if (n.empty() || n[0] == '?' || n.find('/') != std::string::npos) continue;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".png") == 0) names.push_back(n);
    }
    return names;
}



// (path, sha) of each entry of a GitHub git/trees response.
std::vector<std::pair<std::string, std::string>> parseTree(const std::string& json) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t at = 0;
    while (true) {
        std::string path = jsonNext(json, "path", at);
        if (at == std::string::npos) break;
        size_t next = json.find("\"path\"", at), shaAt = at;
        std::string sha = jsonNext(json, "sha", shaAt);
        if (shaAt == std::string::npos || (next != std::string::npos && shaAt > next)) sha.clear();
        out.emplace_back(path, sha);
    }
    return out;
}



std::string libretroUrl(const std::string& repo, const std::string& folder, const std::string& name) {
    std::string spaced = repo;
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    return ART_LIBRETRO_HOST "/" + urlEncode(spaced) + "/" + folder + "/" + urlEncode(name);
}

std::string githubUrl(const std::string& repo, const std::string& branch, const std::string& folder,
                      const std::string& name) {
    return "https://raw.githubusercontent.com/libretro-thumbnails/" + repo + "/" + branch + "/" + folder + "/" +
           urlEncode(name);
}

} // namespace

// ---------------------------------------------------------------- control

void ArtDownload::start(const std::string& appDir, std::vector<System> work) {
    stop();
    m_appDir = appDir;
    m_work = std::move(work);
    m_stop = false;
    m_quitting = false;
    m_netDown = false;
    m_curl.reset();
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_hasResult = false;
        m_result.clear();
        m_status = "Starting";
    }
    m_running = true;
    m_thread = std::thread([this]() { run(); });
}

void ArtDownload::cancel() {
    if (!m_running) return;
    m_stop = true;
    setStatus("Stopping");
    m_curl.cancel();
}

void ArtDownload::stop() {
    m_quitting = true;
    m_stop = true;
    m_curl.cancel();
    if (m_thread.joinable()) m_thread.join();
}

std::string ArtDownload::status() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_status;
}

bool ArtDownload::takeResult(std::string& message) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_hasResult) return false;
    m_hasResult = false;
    message = m_result;
    return true;
}

void ArtDownload::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_status = s;
}

// ---------------------------------------------------------------- curl

int ArtDownload::curl(std::vector<std::string> args, const std::string& stdoutTo) {
    return m_curl.run(std::move(args), stdoutTo);
}

// Downloads `url` to `dest` via a hidden part file (dest's folder, dot name so
// the cover index never lists it). A raw.githubusercontent link to a duplicate
// cover is a git symlink: its body is the real file's name, followed.
bool ArtDownload::fetch(const std::string& url, const std::string& dest, int maxTime, int& rc) {
    std::string dir = dest.substr(0, dest.find_last_of('/'));
    std::string part = dir + "/.download-" + std::to_string((long)::getpid()) + "-" +
                       std::to_string(std::hash<std::string>()(dest)) + ".part";
    std::string u = url;
    for (int hop = 0; hop < 3 && !m_stop; ++hop) {
        ::unlink(part.c_str());
        std::vector<std::string> a = {"--max-time", std::to_string(maxTime), "-o", part, u};
        rc = curl(a);
        if (rc != 0 || !isFile(part)) { ::unlink(part.c_str()); return false; }
        std::string head = readFile(part);
        bool png = head.size() > 8 && std::memcmp(head.data(), "\x89PNG", 4) == 0;
        bool jpg = head.size() > 3 && (unsigned char)head[0] == 0xFF && (unsigned char)head[1] == 0xD8;
        if (png || jpg) {
            // Some source files are cut short (no PNG IEND / JPEG end marker): they
            // don't decode, so they're dropped and remembered as broken.
            const std::string end = head.substr(head.size() > 32 ? head.size() - 32 : 0);
            bool whole = png ? end.find("IEND") != std::string::npos : end.find("\xFF\xD9") != std::string::npos;
            if (!whole) { ::unlink(part.c_str()); rc = kBroken; return false; }
            return ::rename(part.c_str(), dest.c_str()) == 0;
        }
        ::unlink(part.c_str());
        if (head.size() > 512 || head.find('\n') != std::string::npos) return false;
        u = u.substr(0, u.find_last_of('/') + 1) + urlEncode(head);  // symlink target
    }
    ::unlink(part.c_str());
    return false;
}

// The CA bundle and a log line of what this cabinet's curl is (Net.h).
void ArtDownload::setup() {
    m_curl.prepare(m_appDir);
}

// ---------------------------------------------------------------- lists

// The cover (or logo) file names of one libretro-thumbnails set, from
// media/.art-index/ when fetched within a week.
bool ArtDownload::listing(const std::string& repo, const char* folder, Listing& out) {
    std::string dir = m_appDir + "/media/.art-index";
    ::mkdir((m_appDir + "/media").c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    std::string cache = dir + "/" + repo + "-" + folder + ".txt";
    auto load = [&]() {
        std::ifstream f(cache);
        std::string line;
        out = Listing();
        if (!std::getline(f, line) || line.empty() || line[0] != '#') return false;
        out.origin = line.substr(1);
        while (std::getline(f, line))
            if (!line.empty()) out.names.push_back(line);
        return true;
    };
    struct stat st;
    bool cached = ::stat(cache.c_str(), &st) == 0;
    if (cached && std::time(nullptr) - st.st_mtime < kListingMaxAge && load()) return true;

    auto save = [&]() {
        std::ofstream f(cache + ".tmp");
        f << "#" << out.origin << "\n";
        for (const std::string& n : out.names) f << n << "\n";
        f.close();
        ::rename((cache + ".tmp").c_str(), cache.c_str());
    };
    std::string tmp = dir + "/.listing.part";
    auto get = [&](const std::string& url, std::string& body) {
        ::unlink(tmp.c_str());
        std::vector<std::string> a = {"--max-time", "120", "-o", tmp, url};
        if (m_compressed) a.insert(a.begin(), "--compressed");
        int rc = curl(a);
        if (m_compressed && (rc == 2 || rc == 4 || rc == 48)) {  // this curl has no zlib
            m_compressed = false;
            a.erase(a.begin());
            rc = curl(a);
        }
        m_lastExit = rc;
        body = rc == 0 ? readFile(tmp) : "";
        ::unlink(tmp.c_str());
        return rc;
    };
    auto fail = [&](int rc) {
        if (cached && load()) return true;  // a stale list beats none
        if (isNetworkError(rc)) m_netDown = true;
        log("art: no %s list for %s (curl %d)", folder, repo.c_str(), rc);
        return false;
    };

    // thumbnails.libretro.com: one compressed directory page.
    std::string spaced = repo;
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    std::string body;
    int rc = get(ART_LIBRETRO_HOST "/" + urlEncode(spaced) + "/" + folder + "/", body);
    if (rc == 0) {
        out = Listing();
        out.origin = "libretro";
        out.names = parseListing(body);
        if (!out.names.empty()) {
            save();
            log("art: %s/%s: %zu files (libretro)", repo.c_str(), folder, out.names.size());
            return true;
        }
    } else if (rc != 22) {
        log("art: thumbnails.libretro.com failed (curl %d), trying GitHub", rc);
    }

    // Not on that server (or it can't be reached): the GitHub repository, two
    // API calls (the API allows 60 an hour without an account; the week's
    // cache keeps us far below).
    std::string branch;
    for (const char* b : {"master", "main"}) {
        rc = get("https://api.github.com/repos/libretro-thumbnails/" + repo + "/git/trees/" + b, body);
        if (rc == 0) { branch = b; break; }
        if (rc != 22) break;
    }
    if (branch.empty()) return fail(rc);
    std::string sha;
    for (const auto& e : parseTree(body))
        if (e.first == folder) sha = e.second;
    out = Listing();
    out.origin = "github:" + branch;
    if (!sha.empty()) {
        rc = get("https://api.github.com/repos/libretro-thumbnails/" + repo + "/git/trees/" + sha, body);
        if (rc != 0) return fail(rc);
        for (const auto& e : parseTree(body))
            if (e.first.size() > 4 && e.first.compare(e.first.size() - 4, 4, ".png") == 0) out.names.push_back(e.first);
    }
    save();  // an empty list too: this set has none, don't ask again for a week
    log("art: %s/%s: %zu files (GitHub %s)", repo.c_str(), folder, out.names.size(), branch.c_str());
    return true;
}

// The download link of a Bezel Project set's system bezel (the first .png in
// its retroarch/overlay folder), from the GitHub API; remembered in
// media/.art-index/ for a month. "" when there is none or GitHub can't be reached.
std::string ArtDownload::bezelUrl(const std::string& repo) {
    const std::string dir = m_appDir + "/media/.art-index";
    ::mkdir((m_appDir + "/media").c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    const std::string cache = dir + "/bezel-" + repo + ".txt";
    struct stat st;
    if (::stat(cache.c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime < 30 * 24 * 3600) {
        std::string url = readFile(cache);
        while (!url.empty() && (url.back() == '\n' || url.back() == '\r')) url.pop_back();
        return url;
    }
    const std::string tmp = dir + "/.bezel.part";
    ::unlink(tmp.c_str());
    int rc = curl({"--max-time", "60", "-o", tmp,
                   "https://api.github.com/repos/thebezelproject/" + repo + "/contents/retroarch/overlay"});
    std::string body = rc == 0 ? readFile(tmp) : "";
    ::unlink(tmp.c_str());
    if (rc != 0) {
        m_lastExit = rc;
        if (isNetworkError(rc)) m_netDown = true;
        log("art: no bezel list for %s (curl %d)", repo.c_str(), rc);
        return "";
    }
    std::string url;
    size_t at = 0;
    while (url.empty()) {
        std::string name = jsonNext(body, "name", at);
        if (at == std::string::npos) break;
        size_t urlAt = at;
        std::string dl = jsonNext(body, "download_url", urlAt);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0 && urlAt != std::string::npos) url = dl;
    }
    std::ofstream(cache) << url << "\n";  // remembered even when empty: this set has no bezel
    return url;
}

// ---------------------------------------------------------------- the run

void ArtDownload::run() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    ::sigaction(SIGCHLD, &sa, nullptr);  // so waitpid reports curl's exit code
    setup();

    // 1. Which games have no cover / no logo yet (nothing is fetched for the rest).
    setStatus("Checking your games");
    struct Need { size_t game; bool cover, logo; };
    std::vector<std::vector<Need>> needs(m_work.size());
    int coverNeeds = 0, logoNeeds = 0;
    size_t checked = 0, games = 0;
    for (const System& s : m_work) games += s.games.size();
    for (size_t s = 0; s < m_work.size() && !m_stop; ++s) {
        const std::string& id = m_work[s].id;
        for (size_t g = 0; g < m_work[s].games.size() && !m_stop; ++g) {
            if (checked++ % 50 == 0) setStatus("Checking " + std::to_string(checked) + " / " + std::to_string(games));
            const std::vector<std::string>& names = m_work[s].games[g].names;
            bool cover = true, logo = true;
            for (const std::string& n : names) {
                if (!cover && !logo) break;
                if (cover && !findArt(m_appDir, id, n).empty()) cover = false;
                if (logo && !findLogo(m_appDir, id, n).empty()) logo = false;
            }
            if (cover || logo) {
                needs[s].push_back({g, cover, logo});
                coverNeeds += cover;
                logoNeeds += logo;
            }
        }
    }
    // Systems with games and no bezel yet.
    std::vector<size_t> bezelNeeds;
    for (size_t s = 0; s < m_work.size(); ++s)
        if (!m_work[s].games.empty() && bezelRepo(m_work[s].id) && !isFile(m_appDir + "/media/" + m_work[s].id + "/bezel.png"))
            bezelNeeds.push_back(s);
    log("art: %d game(s) without a cover, %d without a logo, %zu system(s) without a bezel", coverNeeds, logoNeeds,
        bezelNeeds.size());

    struct Job { std::vector<std::string> urls; std::string dest; bool logo; };  // urls: tried in order
    // Source files known to be broken (incomplete upstream), skipped so the next
    // best match is used instead.
    const std::string brokenPath = m_appDir + "/media/.art-index/broken.txt";
    std::set<std::string> broken;
    {
        std::ifstream in(brokenPath);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty()) broken.insert(line);
    }
    std::vector<Job> jobs;
    std::set<std::string> queued;
    std::set<std::string> touched;  // systems whose media folders change
    std::vector<std::string> notFound;
    int already = 0;
    std::atomic<int> covers{0}, logos{0}, failed{0}, netFails{0};
    std::atomic<bool> newBroken{false};

    // Twice at most: a file found broken at its source is skipped on the second
    // pass, which takes the next-best match for those games.
    for (int pass = 0; pass < 2 && !m_stop && !m_netDown; ++pass) {
    jobs.clear();
    queued.clear();
    notFound.clear();
    already = 0;
    newBroken = false;

    // 2. Match each against its set's list.
    for (size_t s = 0; s < m_work.size() && !m_stop && !m_netDown; ++s) {
        if (needs[s].empty()) continue;
        const std::string& id = m_work[s].id;
        const Src* src = nullptr;
        for (const Src& x : sources())
            if (id == x.sys) src = &x;
        if (!src) continue;
        const Library::System* sys = Library::findSystem(id);
        setStatus("Looking up " + (sys ? sys->name : id));
        std::vector<bool> coverFound(needs[s].size(), false);
        for (int kind = 0; kind < 2 && !m_stop && !m_netDown; ++kind) {
            const bool logo = kind == 1;
            const char* folder = logo ? "Named_Logos" : "Named_Boxarts";
            const std::vector<const char*>& repos = logo ? src->logos : src->covers;
            const std::string destSys = logo ? src->logoSys : src->sys;
            bool any = false;
            for (const Need& n : needs[s]) any = any || (logo ? n.logo : n.cover);
            if (!any) continue;
            // Every set's names in one index; the first set listing a name owns it.
            std::map<std::string, std::pair<std::string, std::string>> owner;  // name -> (repo, origin)
            std::vector<std::string> all;
            std::string key = "@net/";
            for (const char* repo : repos) {
                key += std::string(repo) + "+";
                Listing l;
                if (!listing(repo, folder, l)) continue;
                for (const std::string& n : l.names)
                    if (!broken.count(std::string(folder) + "/" + n) &&
                        owner.emplace(n, std::make_pair(std::string(repo), l.origin)).second)
                        all.push_back(n);
            }
            if (all.empty()) continue;
            setArtIndex(key + "/" + folder, all);
            for (size_t i = 0; i < needs[s].size(); ++i) {
                const Need& n = needs[s][i];
                if (!(logo ? n.logo : n.cover)) continue;
                std::string hit;
                for (const std::string& name : m_work[s].games[n.game].names) {
                    std::string stem = name.substr(0, name.find_last_of('.')), safe = stem;
                    for (char& ch : safe)
                        if (std::strchr("&*/:`<>?\\|\"", ch)) ch = '_';
                    if (owner.count(stem + ".png")) hit = stem + ".png";
                    else if (owner.count(safe + ".png")) hit = safe + ".png";
                    else hit = matchCover(key + "/", stem, folder);
                    if (!hit.empty()) break;
                }
                if (hit.empty()) {
                    if (!logo) notFound.push_back(id + "\t" + m_work[s].games[n.game].names[0]);
                    continue;
                }
                if (!logo) coverFound[i] = true;
                std::string destDir = m_appDir + "/media/" + destSys + "/" + folder;
                std::string dest = destDir + "/" + hit;
                if (isFile(dest)) { ++already; continue; }
                if (!queued.insert(dest).second) continue;
                ::mkdir((m_appDir + "/media/" + destSys).c_str(), 0755);
                ::mkdir(destDir.c_str(), 0755);
                const auto& own = owner[hit];
                Job j;
                j.dest = dest;
                j.logo = logo;
                if (own.second == "libretro") {
                    j.urls = {libretroUrl(own.first, folder, hit), githubUrl(own.first, "master", folder, hit),
                              githubUrl(own.first, "main", folder, hit)};
                } else {
                    j.urls = {githubUrl(own.first, own.second.substr(7), folder, hit)};
                }
                jobs.push_back(j);
                touched.insert(destSys);
            }
            setArtIndex(key + "/" + folder, {});  // the list's index is done with
        }
    }

    // 3. Download, a few at a time.
    std::atomic<size_t> next{0};
    std::atomic<int> done{0};
    const size_t total = jobs.size();
    if (total) setStatus("0 / " + std::to_string(total));
    log("art: %zu file(s) to download, %d already here", total, already);
    auto worker = [&]() {
        while (!m_stop && !m_netDown) {
            size_t i = next++;
            if (i >= total) break;
            const Job& j = jobs[i];
            // The other server when one fails; a network error counts only when
            // every source failed with one.
            int rc = 0;
            bool ok = false, allNet = true, isBroken = false;
            for (size_t u = 0; u < j.urls.size() && !ok && !m_stop; ++u) {
                ok = fetch(j.urls[u], j.dest, 60, rc);
                if (!ok && !isNetworkError(rc)) allNet = false;
                if (rc == kBroken) isBroken = true;
            }
            if (ok) {
                (j.logo ? logos : covers)++;
                netFails = 0;
            } else if (isBroken) {
                const std::string name = j.dest.substr(j.dest.find_last_of('/') + 1);
                const std::string folder = j.logo ? "Named_Logos" : "Named_Boxarts";
                log("art: broken at the source, skipped from now on: %s/%s", folder.c_str(), name.c_str());
                std::lock_guard<std::mutex> lock(m_mu);
                std::ofstream(brokenPath, std::ios::app) << folder << "/" << name << "\n";
                broken.insert(folder + "/" + name);
                newBroken = true;
            } else if (!m_stop) {
                ++failed;
                log("art: failed (curl %d): %s", rc, j.urls[0].c_str());
                // Many network errors in a row with nothing working: the connection is gone.
                if (allNet && ++netFails >= 8) { m_lastExit = rc; m_netDown = true; }
            }
            int d = ++done;
            setStatus(std::to_string(d) + " / " + std::to_string(total));
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < kParallel && t < (int)total; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    if (!newBroken) break;
    log("art: looking again for the games whose file was broken");
    }

    // 4. Bezels (The Bezel Project), one per system: the frame around the game.
    std::atomic<int> bezels{0};
    for (size_t s : bezelNeeds) {
        if (m_stop || m_netDown) break;
        const std::string& id = m_work[s].id;
        const Library::System* sys = Library::findSystem(id);
        setStatus("Bezel for " + (sys ? sys->name : id));
        const std::string url = bezelUrl(bezelRepo(id));
        if (url.empty()) continue;
        ::mkdir((m_appDir + "/media/" + id).c_str(), 0755);
        int rc = 0;
        if (fetch(url, m_appDir + "/media/" + id + "/bezel.png", 120, rc)) {
            ++bezels;
            log("art: bezel for %s", id.c_str());
        } else if (!m_stop) {
            ++failed;
            log("art: bezel for %s failed (curl %d)", id.c_str(), rc);
            if (isNetworkError(rc)) m_lastExit = rc;
        }
    }

    // 5. The menu sees the new files (not when it is closing: it rereads them).
    if (!m_quitting)
        for (const std::string& sys : touched) refreshArtIndex(m_appDir, sys);

    // Games no set has a cover for, for anyone wondering (rewritten after a full run).
    if (!m_stop) {
        std::ofstream f(m_appDir + "/media/art-not-found.txt");
        f << "# Games Settings > Download artwork found no cover for (system, ROM).\n";
        for (const std::string& n : notFound) f << n << "\n";
    }

    std::string msg;
    if (m_netDown && covers + logos + bezels == 0) msg = Net::message(m_lastExit, "the artwork server");
    else if (m_stop) msg = "Artwork download stopped";
    else if (coverNeeds + logoNeeds == 0 && bezelNeeds.empty()) msg = "Every game already has artwork";
    else if (covers + logos + bezels == 0 && !failed)
        msg = "No new artwork" + (notFound.empty() ? std::string() : " - " + std::to_string(notFound.size()) +
                                  " games have no cover online");
    else {
        msg = "Artwork: " + std::to_string(covers.load()) + " covers, " + std::to_string(logos.load()) + " logos";
        if (bezels) msg += ", " + std::to_string(bezels.load()) + (bezels == 1 ? " bezel" : " bezels");
        msg += " added";
        if (failed) msg += ", " + std::to_string(failed.load()) + " failed";
        else if (!notFound.empty()) msg += ", " + std::to_string(notFound.size()) + " not found";
        if (m_netDown) msg += " (connection lost)";
    }
    log("art: %s (%zu not found)", msg.c_str(), notFound.size());
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_result = msg;
        m_hasResult = true;
        m_status.clear();
    }
    m_running = false;
}
