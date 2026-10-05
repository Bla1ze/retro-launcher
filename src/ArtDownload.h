#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

// Settings > Download artwork: covers and logos for the games on the stick that
// have none yet, from libretro-thumbnails, and a bezel for each system with
// games (The Bezel Project), on a worker thread.
//
//   - Games that already have a cover / logo are skipped before anything is
//     fetched; with nothing missing, no network is used at all.
//   - A system's list of covers comes from thumbnails.libretro.com (one small
//     compressed page), or, for sets that server lacks (arcade logos and a few
//     more), from the GitHub API. Lists are kept a week in media/.art-index/,
//     so running it again only downloads files.
//   - Only the matched files are downloaded, four at a time, saved as PNG
//     beside the prefilled covers (media/<system>/Named_Boxarts, Named_Logos).
//
// Downloads go through the firmware's curl, as the SDK's own HTTP helper does.
class ArtDownload {
public:
    struct Game { std::vector<std::string> names; };  // ROM-style file names to match, best first
    struct System { std::string id; std::vector<Game> games; };

    ~ArtDownload() { stop(); }
    void start(const std::string& appDir, std::vector<System> work);
    void cancel();  // stops soon, without waiting (running() until it has)
    void stop();    // cancels and waits (curl processes are killed); for shutdown
    bool running() const { return m_running.load(); }
    std::string status() const;           // "Covers 34 / 210" while running
    bool takeResult(std::string& message);  // once, when a run has ended

private:
    void run();
    struct Listing { std::string origin; std::vector<std::string> names; };  // origin "libretro" / "github:<branch>"
    bool listing(const std::string& repo, const char* folder, Listing& out);
    std::string bezelUrl(const std::string& repo);
    void setup();
    int curl(std::vector<std::string> args, const std::string& stdoutTo = "");
    bool fetch(const std::string& url, const std::string& dest, int maxTime, int& rc);
    void setStatus(const std::string& s);

    std::string m_appDir;
    std::vector<System> m_work;
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_stop{false};
    std::atomic<bool> m_netDown{false}, m_quitting{false};
    std::string m_caBundle;  // data/ca-bundle.pem
    bool m_compressed = true;  // drop --compressed if this curl lacks zlib

    mutable std::mutex m_mu;
    std::string m_status, m_result;
    bool m_hasResult = false;
    std::set<pid_t> m_children;
    std::atomic<int> m_lastExit{0};  // curl's last failing exit code, for the message
};
