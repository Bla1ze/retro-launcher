#pragma once

#include "Net.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Settings > Updates: compares this build with the latest GitHub release of
// Bla1ze/retro-launcher, and on request downloads its zip and replaces only the
// files that differ (size + CRC-32), each written to a hidden part file first
// and renamed into place once every file has been extracted and checked. The
// menu then restarts into the new version.
//
// Only what a release ships is touched (the app, cores, PPSSPP files, licenses,
// console pictures): games, saves, settings, downloaded artwork and BIOS files
// are never in a release. The app's .png / .xml are left alone if present.
class Updater {
public:
    enum class State { Idle, Checking, UpToDate, NoRelease, Available, Downloading, Installing, Done, Failed };

    ~Updater();
    // Checks in the background. `quiet`: the automatic daily check at startup.
    void check(const std::string& appDir, bool quiet);
    void install();  // after check() said Available
    void stop();

    State state() const { return m_state.load(); }
    bool quiet() const { return m_quiet; }
    std::string latest() const;   // "0.23.0"
    std::string message() const;  // failure reason, or what was done
    int progress() const;         // download percent while Downloading
    // What's new in every release between this build and the latest, newest
    // first: (version, notes as Markdown, the release's install section and
    // checksum left out). Shown before installing.
    std::vector<std::pair<std::string, std::string>> notes() const;
    int filesUpdated() const { return m_updated.load(); }
    // The startup check is due: no answer from GitHub in the last hour (the
    // menu starts again after every game, so not every one of those).
    static bool startupCheckDue(const std::string& appDir);
    // The newest version the last successful check found ("" if none yet):
    // keeps "update available" showing after the menu restarts, until installed.
    static std::string knownLatest(const std::string& appDir);
    // "1.0.10" is newer than "1.0.9" (numbers only; a "-test" suffix is ignored).
    static bool isNewer(const std::string& version, const std::string& than);

private:
    void runCheck();
    void runInstall();
    bool apply(const std::string& zipPath, std::string& why);
    void fail(const std::string& why);

    std::string m_appDir;
    std::thread m_thread;
    Net::Curl m_curl;
    std::atomic<State> m_state{State::Idle};
    std::atomic<int> m_updated{0};
    bool m_quiet = false;
    bool m_prepared = false;
    mutable std::mutex m_mu;
    std::string m_latest, m_url, m_message, m_partPath;
    std::vector<std::pair<std::string, std::string>> m_notes;
    uint64_t m_size = 0;
};
