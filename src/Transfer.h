#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Settings > Wi-Fi transfer: a small web server, only while that screen is open,
// so games can be sent from any computer or phone on the same network straight
// into roms/<system>/. A browser page (built in) lists the systems; files are
// uploaded one at a time with a PUT per file.
//
// Safety: every API call needs the 4-digit PIN shown on the cabinet, in an
// X-Pin header (a custom header, so other web pages can't send it: no CORS is
// ever answered). Names are checked (no .., one folder level only where a
// system takes game folders) and limited to the system's own file types.
// Uploads go to a hidden .part file, synced and renamed when complete, and
// deleted if anything goes wrong.
class TransferServer {
public:
    struct Status {
        bool listening = false;
        std::string error;                 // why it isn't listening
        std::vector<std::string> addresses;  // "192.168.1.20" (every IPv4 interface that is up)
        int port = 0;
        std::string pin;
        std::string current;               // file being received, or ""
        uint64_t currentDone = 0, currentTotal = 0;
        int received = 0;                  // files completed since the screen opened
        uint64_t receivedBytes = 0;
        std::vector<std::string> recent;   // last few results, newest first ("NES: Zelda.nes")
        int connections = 0;
    };

    ~TransferServer() { stop(); }
    bool start(const std::string& appDir);
    void stop();  // closes everything, including uploads in progress
    bool running() const { return m_running.load(); }
    Status status() const;

private:
    void acceptLoop();
    void serve(int fd);
    void note(const std::string& line);

    std::string m_appDir;
    int m_listen = -1;
    std::atomic<bool> m_running{false}, m_stop{false};
    std::thread m_acceptThread;
    std::list<std::thread> m_threads;
    std::atomic<int> m_active{0};
    std::atomic<int> m_badPins{0};

    mutable std::mutex m_mu;
    Status m_status;
    std::set<int> m_fds;  // open connections, shut down by stop()
};
