#include "Update.h"

#include "Library.h"
#include "Version.h"
#include "vendor/stb_image.h"  // stbi_zlib_decode_noheader_buffer (stb's implementation lives in GamePanels.cpp)

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using Library::log;

namespace {

const char* kLatestRelease = "https://api.github.com/repos/Bla1ze/retro-launcher/releases/latest";
const char* kPrefix = "external/retro-launcher/";

std::vector<int> versionParts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> out;
    for (size_t at = 0; at < v.size();) {
        size_t dot = v.find('.', at);
        out.push_back(std::atoi(v.substr(at, dot == std::string::npos ? std::string::npos : dot - at).c_str()));
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
    while (out.size() < 3) out.push_back(0);
    return out;
}

bool isNewer(const std::string& latest, const std::string& current) {
    return versionParts(latest) > versionParts(current);
}

uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// CRC-32 of a file on the stick, or false if it can't be read.
bool crcFile(const std::string& path, uint32_t& crc, uint64_t& size) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf(1 << 20);
    crc = 0;
    size = 0;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        crc = crcUpdate(crc, buf.data(), n);
        size += n;
    }
    std::fclose(f);
    return true;
}

uint32_t rd32(const std::string& s, size_t at) {
    const uint8_t* p = (const uint8_t*)s.data() + at;
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t rd16(const std::string& s, size_t at) {
    const uint8_t* p = (const uint8_t*)s.data() + at;
    return (uint16_t)(p[0] | (p[1] << 8));
}

void makeDirs(const std::string& dir) {
    for (size_t at = 1; at <= dir.size(); ++at)
        if (at == dir.size() || dir[at] == '/') ::mkdir(dir.substr(0, at).c_str(), 0755);
}

// The number after "key": in a JSON object, from `at`.
uint64_t jsonNumber(const std::string& s, const char* key, size_t at) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k, at);
    if (p == std::string::npos) return 0;
    p += k.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == ':')) ++p;
    return std::strtoull(s.c_str() + p, nullptr, 10);
}

} // namespace

Updater::~Updater() { stop(); }

void Updater::stop() {
    m_curl.cancel();
    if (m_thread.joinable()) m_thread.join();
}

bool Updater::dailyCheckDue(const std::string& appDir) {
    struct stat st;
    return ::stat((appDir + "/data/update-check.txt").c_str(), &st) != 0 || std::time(nullptr) - st.st_mtime > 24 * 3600;
}

std::string Updater::latest() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_latest;
}

std::string Updater::message() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_message;
}

int Updater::progress() const {
    std::string part;
    uint64_t total;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        part = m_partPath;
        total = m_size;
    }
    struct stat st;
    if (part.empty() || !total || ::stat(part.c_str(), &st) != 0) return 0;
    return (int)std::min<uint64_t>(100, (uint64_t)st.st_size * 100 / total);
}

void Updater::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_message = why;
    }
    log("update: %s", why.c_str());
    m_state = State::Failed;
}

void Updater::check(const std::string& appDir, bool quiet) {
    const State s = m_state;
    if (s == State::Checking || s == State::Downloading || s == State::Installing) return;
    if (m_thread.joinable()) m_thread.join();
    m_appDir = appDir;
    m_quiet = quiet;
    m_curl.reset();
    m_state = State::Checking;
    m_thread = std::thread([this]() { runCheck(); });
}

void Updater::install() {
    if (m_state != State::Available) return;
    if (m_thread.joinable()) m_thread.join();
    m_quiet = false;
    m_curl.reset();
    m_state = State::Downloading;
    m_thread = std::thread([this]() { runInstall(); });
}

void Updater::runCheck() {
    if (!m_prepared) { m_curl.prepare(m_appDir); m_prepared = true; }
    ::mkdir((m_appDir + "/data").c_str(), 0755);
    std::ofstream(m_appDir + "/data/update-check.txt") << std::time(nullptr) << "\n";  // the daily check's clock
    const std::string tmp = m_appDir + "/data/.release.json", codeFile = m_appDir + "/data/.release.code";
    int rc = m_curl.run({"--max-time", "30", "-o", tmp, "-w", "%{http_code}", kLatestRelease}, codeFile, false);
    const std::string body = rc == 0 ? Net::readFile(tmp) : "";
    const int http = std::atoi(Net::readFile(codeFile).c_str());
    ::unlink(tmp.c_str());
    ::unlink(codeFile.c_str());
    if (rc != 0) { fail(Net::message(rc, "GitHub")); return; }
    if (http == 404) {  // nothing published yet
        log("update: no release found");
        m_state = State::NoRelease;
        return;
    }
    if (http == 403 || http == 429) { fail("GitHub is busy - try again in an hour"); return; }
    if (http != 200) { fail("GitHub answered " + std::to_string(http)); return; }
    size_t at = 0;
    std::string tag = Net::jsonNext(body, "tag_name", at);
    if (tag.empty()) { fail("GitHub's answer had no version"); return; }
    // The release zip among its files.
    std::string url;
    uint64_t size = 0;
    at = 0;
    while (true) {
        std::string name = Net::jsonNext(body, "name", at);
        if (at == std::string::npos) break;
        if (name.compare(0, 16, "retro-launcher-v") == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
            size_t u = at;
            url = Net::jsonNext(body, "browser_download_url", u);
            size = jsonNumber(body, "size", at);
            break;
        }
    }
    std::string version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_latest = version;
        m_url = url;
        m_size = size;
    }
    if (!isNewer(version, APP_VERSION)) {
        log("update: v%s is the latest (this is v%s)", version.c_str(), APP_VERSION);
        m_state = State::UpToDate;
        return;
    }
    if (url.empty()) { fail("v" + version + " has no download"); return; }
    log("update: v%s available (%llu bytes): %s", version.c_str(), (unsigned long long)size, url.c_str());
    m_state = State::Available;
}

void Updater::runInstall() {
    std::string url, version;
    uint64_t size;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        url = m_url;
        version = m_latest;
        size = m_size;
        m_partPath = m_appDir + "/data/.update-v" + version + ".zip";
    }
    const std::string part = m_appDir + "/data/.update-v" + version + ".zip";
    ::unlink(part.c_str());
    int rc = m_curl.run({"--max-time", "3600", "-o", part, url});
    struct stat st;
    if (rc != 0 || ::stat(part.c_str(), &st) != 0 || (size && (uint64_t)st.st_size != size)) {
        ::unlink(part.c_str());
        fail(rc == -2 ? "Update stopped" : rc ? Net::message(rc, "GitHub") : "The download was incomplete");
        return;
    }
    m_state = State::Installing;
    std::string why;
    bool ok = apply(part, why);
    ::unlink(part.c_str());
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_partPath.clear();
    }
    if (!ok) { fail(why); return; }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_message = "Updated " + std::to_string(m_updated.load()) + " files to v" + version;
    }
    log("update: %s", m_message.c_str());
    m_state = State::Done;
}

// Extracts the files of the release zip that differ from the stick's, each to
// a part file beside it, then renames them all into place (the app last).
bool Updater::apply(const std::string& zipPath, std::string& why) {
    const std::string zip = Net::readFile(zipPath);
    if (zip.size() < 22) { why = "The download is not a zip"; return false; }
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22 + 1; i-- > (zip.size() > 65557 ? zip.size() - 65557 : 0);)
        if (rd32(zip, i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) { why = "The download is damaged (no zip directory)"; return false; }
    const size_t count = rd16(zip, eocd + 10), cdOff = rd32(zip, eocd + 16);
    struct Pending { std::string part, dest; };
    std::vector<Pending> pending;
    auto discard = [&]() {
        for (const Pending& p : pending) ::unlink(p.part.c_str());
    };
    size_t p = cdOff, unchanged = 0;
    for (size_t i = 0; i < count; ++i) {
        if (p + 46 > zip.size() || rd32(zip, p) != 0x02014b50) { discard(); why = "The download is damaged"; return false; }
        const uint16_t method = rd16(zip, p + 10);
        const uint32_t crc = rd32(zip, p + 16), csize = rd32(zip, p + 20), usize = rd32(zip, p + 24);
        const uint16_t nlen = rd16(zip, p + 28), xlen = rd16(zip, p + 30), clen = rd16(zip, p + 32);
        const uint32_t local = rd32(zip, p + 42);
        const std::string name = zip.substr(p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (name.compare(0, std::strlen(kPrefix), kPrefix) != 0 || name.back() == '/') continue;  // INSTALL.txt, folders
        const std::string rel = name.substr(std::strlen(kPrefix));
        // Only what a release ships, and nothing outside the app folder.
        const std::string top = rel.substr(0, rel.find('/'));
        bool allowed = top == "retro-launcher.elf" || top == "retro-launcher.xml" || top == "retro-launcher.png" ||
                       top == "cores" || top == "system" || top == "licenses" || top == "media";
        if (!allowed || rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos || rel[0] == '/') {
            log("update: skipped %s", name.c_str());
            continue;
        }
        const std::string dest = m_appDir + "/" + rel;
        struct stat st;
        if ((rel == "retro-launcher.png" || rel == "retro-launcher.xml") && ::stat(dest.c_str(), &st) == 0) continue;  // the owner's own
        uint32_t haveCrc;
        uint64_t haveSize;
        if (crcFile(dest, haveCrc, haveSize) && haveSize == usize && haveCrc == crc) { ++unchanged; continue; }
        // Extract and check it.
        if (local + 30 > zip.size() || rd32(zip, local) != 0x04034b50) { discard(); why = "The download is damaged"; return false; }
        const size_t data = local + 30 + rd16(zip, local + 26) + rd16(zip, local + 28);
        if (data + csize > zip.size()) { discard(); why = "The download is damaged"; return false; }
        std::string out;
        if (method == 0) {
            out = zip.substr(data, csize);
        } else if (method == 8) {
            out.resize(usize);
            int got = usize ? stbi_zlib_decode_noheader_buffer(&out[0], (int)usize, zip.data() + data, (int)csize) : 0;
            if (got != (int)usize) { discard(); why = "Could not unpack " + rel; return false; }
        } else {
            discard();
            why = "Unsupported compression in " + rel;
            return false;
        }
        if (crcUpdate(0, (const uint8_t*)out.data(), out.size()) != crc) { discard(); why = "Checksum mismatch in " + rel; return false; }
        const std::string dir = dest.substr(0, dest.find_last_of('/'));
        makeDirs(dir);
        const std::string part = dir + "/.update-" + dest.substr(dest.find_last_of('/') + 1) + ".part";
        {
            std::ofstream f(part, std::ios::binary);
            f.write(out.data(), (std::streamsize)out.size());
            f.flush();
            if (!f) { discard(); ::unlink(part.c_str()); why = "Could not write " + rel + " (stick full?)"; return false; }
        }
        pending.push_back({part, dest});
    }
    // Everything is unpacked and checked: put it in place, the app itself last.
    std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        auto isApp = [](const Pending& x) { return x.dest.size() >= 18 && x.dest.compare(x.dest.size() - 18, 18, "retro-launcher.elf") == 0; };
        return !isApp(a) && isApp(b);
    });
    ::sync();
    int done = 0;
    for (const Pending& q : pending) {
        if (::rename(q.part.c_str(), q.dest.c_str()) != 0) {
            log("update: could not replace %s: %s", q.dest.c_str(), std::strerror(errno));
            ::unlink(q.part.c_str());
            continue;
        }
        ++done;
        if (q.dest.size() >= 18 && q.dest.compare(q.dest.size() - 18, 18, "retro-launcher.elf") == 0) ::chmod(q.dest.c_str(), 0755);
    }
    ::sync();
    m_updated = done;
    log("update: %d file(s) replaced, %zu unchanged", done, unchanged);
    if (done < (int)pending.size()) { why = "Only " + std::to_string(done) + " of " + std::to_string(pending.size()) + " files could be replaced"; return false; }
    return true;
}
