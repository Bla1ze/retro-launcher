#include "ArtDownload.h"

#include "GamePanels.h"
#include "Library.h"

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

namespace {

const char* kUserAgent = "retro-launcher (https://github.com/Bla1ze/retro-launcher)";
const long kListingMaxAge = 7 * 24 * 3600;  // a week, like tools/prefill_boxart.py
const int kParallel = 4;

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

bool isFile(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
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

// A JSON string starting at s[at] == '"'; `at` ends past its closing quote.
std::string jsonString(const std::string& s, size_t& at) {
    std::string out;
    for (++at; at < s.size() && s[at] != '"'; ++at) {
        if (s[at] != '\\') { out += s[at]; continue; }
        if (++at >= s.size()) break;
        char c = s[at];
        if (c == 'u' && at + 4 < s.size()) {
            unsigned cp = (unsigned)std::strtoul(s.substr(at + 1, 4).c_str(), nullptr, 16);
            at += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && at + 6 < s.size() && s[at + 1] == '\\' && s[at + 2] == 'u') {
                unsigned lo = (unsigned)std::strtoul(s.substr(at + 3, 4).c_str(), nullptr, 16);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                at += 6;
            }
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
            else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 63)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
        } else {
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
        }
    }
    ++at;
    return out;
}

// The value of the next `"key": "..."` from `at`, or "" (at = npos when none).
std::string jsonNext(const std::string& s, const char* key, size_t& at) {
    const std::string k = std::string("\"") + key + "\"";
    at = s.find(k, at);
    if (at == std::string::npos) return "";
    at += k.size();
    while (at < s.size() && (s[at] == ' ' || s[at] == ':' || s[at] == '\n' || s[at] == '\t')) ++at;
    if (at >= s.size() || s[at] != '"') return "";
    return jsonString(s, at);
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

bool isNetworkError(int rc) {
    // couldn't resolve / connect, timeout, TLS, empty reply, send/receive errors
    return rc == 5 || rc == 6 || rc == 7 || rc == 28 || rc == 35 || rc == 52 || rc == 55 || rc == 56 ||
           rc == 60 || rc == 77 || rc < 0 || rc == 127;
}

std::string netMessage(int rc) {
    if (rc < 0 || rc == 127) return "Can't download: curl isn't available";
    if (rc == 6 || rc == 5) return "No internet connection (can't look up the server)";
    if (rc == 7 || rc == 28) return "Couldn't reach the artwork server";
    if (rc == 35 || rc == 60 || rc == 77) return "Secure connection failed (curl " + std::to_string(rc) + ")";
    return "Download failed (curl " + std::to_string(rc) + ")";
}

std::string libretroUrl(const std::string& repo, const std::string& folder, const std::string& name) {
    std::string spaced = repo;
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    return "https://thumbnails.libretro.com/" + urlEncode(spaced) + "/" + folder + "/" + urlEncode(name);
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
    m_netDown = false;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_hasResult = false;
        m_result.clear();
        m_status = "Starting";
    }
    m_running = true;
    m_thread = std::thread([this]() { run(); });
}

void ArtDownload::stop() {
    m_stop = true;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        for (pid_t p : m_children) ::kill(p, SIGTERM);
    }
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

// Runs curl with `args`; its exit code, or -1 when it can't be started. The
// menu may run with signals blocked or ignored (the watchdog), so the child
// gets a clean signal mask and default handlers.
int ArtDownload::curl(std::vector<std::string> args) {
    static const char* path = []() -> const char* {
        for (const char* p : {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl"})
            if (::access(p, X_OK) == 0) return p;
        return nullptr;
    }();
    static const bool haveCa = ::access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0;
    std::vector<std::string> full = {"curl", "-sfL", "--connect-timeout", "15", "--retry", "2", "-A", kUserAgent};
    if (haveCa) { full.push_back("--cacert"); full.push_back("/etc/ssl/certs/ca-certificates.crt"); }
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : full) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    sigdelset(&all, SIGKILL);
    sigdelset(&all, SIGSTOP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

    pid_t pid = -1;
    int err;
    {
        std::lock_guard<std::mutex> lock(m_mu);  // so stop() sees every child
        if (m_stop) err = ECANCELED;
        else {
            err = path ? posix_spawn(&pid, path, &fa, &attr, argv.data(), environ)
                       : posix_spawnp(&pid, "curl", &fa, &attr, argv.data(), environ);
            if (err == 0) m_children.insert(pid);
        }
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (err != 0) {
        if (err != ECANCELED) log("art: can't run curl: %s", std::strerror(err));
        return -1;
    }
    int st = 0;
    pid_t w;
    do w = ::waitpid(pid, &st, 0); while (w < 0 && errno == EINTR);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_children.erase(pid);
    }
    if (w < 0) return 0;  // SIGCHLD ignored: the child was reaped for us; callers check the file
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -2;  // killed (stop)
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
        if (png || jpg) return ::rename(part.c_str(), dest.c_str()) == 0;
        ::unlink(part.c_str());
        if (head.size() > 512 || head.find('\n') != std::string::npos) return false;
        u = u.substr(0, u.find_last_of('/') + 1) + urlEncode(head);  // symlink target
    }
    ::unlink(part.c_str());
    return false;
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
        if (isNetworkError(rc)) m_netDown = true;
        if (cached && load()) return true;  // a stale list beats none
        log("art: no %s list for %s (curl %d)", folder, repo.c_str(), rc);
        return false;
    };

    // thumbnails.libretro.com: one compressed directory page.
    std::string spaced = repo;
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    std::string body;
    int rc = get("https://thumbnails.libretro.com/" + urlEncode(spaced) + "/" + folder + "/", body);
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
        return fail(rc);
    }

    // Not on that server: the GitHub repository, two API calls (the API allows
    // 60 an hour without an account; the week's cache keeps us far below).
    std::string branch;
    for (const char* b : {"master", "main"}) {
        rc = get("https://api.github.com/repos/libretro-thumbnails/" + repo + "/git/trees/" + b, body);
        if (rc == 0) { branch = b; break; }
        if (rc != 22) return fail(rc);
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

// ---------------------------------------------------------------- the run

void ArtDownload::run() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    ::sigaction(SIGCHLD, &sa, nullptr);  // so waitpid reports curl's exit code

    // 1. Which games have no cover / no logo yet (nothing is fetched for the rest).
    setStatus("Checking your games");
    struct Need { size_t game; bool cover, logo; };
    std::vector<std::vector<Need>> needs(m_work.size());
    int coverNeeds = 0, logoNeeds = 0;
    for (size_t s = 0; s < m_work.size() && !m_stop; ++s) {
        const std::string& id = m_work[s].id;
        for (size_t g = 0; g < m_work[s].games.size() && !m_stop; ++g) {
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
    log("art: %d game(s) without a cover, %d without a logo", coverNeeds, logoNeeds);

    struct Job { std::string url, fallback, dest; bool logo; };
    std::vector<Job> jobs;
    std::set<std::string> queued;
    std::set<std::string> touched;  // systems whose media folders change
    std::vector<std::string> notFound;
    int already = 0;

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
                    if (owner.emplace(n, std::make_pair(std::string(repo), l.origin)).second) all.push_back(n);
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
                    j.url = libretroUrl(own.first, folder, hit);
                    j.fallback = githubUrl(own.first, "master", folder, hit);
                } else {
                    j.url = githubUrl(own.first, own.second.substr(7), folder, hit);
                }
                jobs.push_back(j);
                touched.insert(destSys);
            }
            setArtIndex(key + "/" + folder, {});  // the list's index is done with
        }
    }

    // 3. Download, a few at a time.
    std::atomic<size_t> next{0};
    std::atomic<int> done{0}, covers{0}, logos{0}, failed{0}, netFails{0};
    const size_t total = jobs.size();
    if (total) setStatus("0 / " + std::to_string(total));
    log("art: %zu file(s) to download, %d already here", total, already);
    auto worker = [&]() {
        while (!m_stop && !m_netDown) {
            size_t i = next++;
            if (i >= total) break;
            const Job& j = jobs[i];
            int rc = 0, rc2 = 0;
            bool ok = fetch(j.url, j.dest, 60, rc);
            if (!ok && !m_stop && !j.fallback.empty() && !isNetworkError(rc)) ok = fetch(j.fallback, j.dest, 60, rc2);
            if (ok) {
                (j.logo ? logos : covers)++;
                netFails = 0;
            } else if (!m_stop) {
                ++failed;
                log("art: failed (curl %d): %s", rc, j.url.c_str());
                // Many network errors in a row with nothing working: the connection is gone.
                if (isNetworkError(rc) && ++netFails >= 8) { m_lastExit = rc; m_netDown = true; }
            }
            int d = ++done;
            setStatus(std::to_string(d) + " / " + std::to_string(total));
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < kParallel && t < (int)total; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();

    // 4. The menu sees the new files.
    for (const std::string& sys : touched) refreshArtIndex(m_appDir, sys);

    // Games no set has a cover for, for anyone wondering (rewritten each run).
    {
        std::ofstream f(m_appDir + "/media/art-not-found.txt");
        f << "# Games Settings > Download artwork found no cover for (system, ROM).\n";
        for (const std::string& n : notFound) f << n << "\n";
    }

    std::string msg;
    if (m_netDown && covers + logos == 0) msg = netMessage(m_lastExit);
    else if (m_stop) msg = "Artwork download stopped";
    else if (coverNeeds + logoNeeds == 0) msg = "Every game already has artwork";
    else if (covers + logos == 0 && !failed)
        msg = "No new artwork" + (notFound.empty() ? std::string() : " - " + std::to_string(notFound.size()) +
                                  " games have no cover online");
    else {
        msg = "Artwork: " + std::to_string(covers.load()) + " covers, " + std::to_string(logos.load()) + " logos added";
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
