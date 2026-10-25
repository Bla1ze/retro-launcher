#pragma once

// CPU pinning. The 4KP's fast Cortex-A76 cores are cpu1-4 and its slow A55s
// are cpu0 and cpu5-7, roughly 2.4x apart. Unpinned, the scheduler ran a
// Saturn game on a slow core over a third of the time: 55.7 fps with 6.4 audio
// underruns a second, against 59.7 fps and 0.12 pinned (1.0.6-pintest,
// 2026-10-12). This finds the two groups from each core's top clock (so it
// doesn't depend on the numbering), pins the emulator to the fast group and
// the panel worker to the slow one, and lets the player log where the
// emulator actually ran.

#include <pthread.h>
#include <sched.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace CpuPin {

struct Groups {
    std::vector<int> fast, slow;
};

// Cores with the highest cpuinfo_max_freq are "fast"; the rest "slow". On a
// single-cluster machine everything is fast and slow is empty.
inline const Groups& groups()
{
    static Groups g;
    static bool done = false;
    if (done) return g;
    done = true;
    std::vector<long> freq;
    long top = 0;
    for (int cpu = 0; cpu < 16; ++cpu) {
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        FILE* f = std::fopen(path, "r");
        if (!f) break;
        long v = 0;
        if (std::fscanf(f, "%ld", &v) != 1) v = 0;
        std::fclose(f);
        freq.push_back(v);
        if (v > top) top = v;
    }
    for (size_t i = 0; i < freq.size(); ++i) (freq[i] == top ? g.fast : g.slow).push_back(static_cast<int>(i));
    return g;
}

inline std::string list(const std::vector<int>& cores)
{
    std::string s;
    for (int c : cores) s += (s.empty() ? "" : " ") + std::to_string(c);
    return s.empty() ? "-" : s;
}

inline bool pinSelf(const std::vector<int>& cores)
{
    if (cores.empty()) return false;
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cores) CPU_SET(c, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    return false;  // the Mac simulator: no affinity
#endif
}

// The core this thread is on, or -1 where that can't be asked (the Mac simulator).
inline int currentCpu()
{
#ifdef __linux__
    return sched_getcpu();
#else
    return -1;
#endif
}

inline bool isFast(int cpu)
{
    for (int c : groups().fast) if (c == cpu) return true;
    return false;
}

// Settings > CPU pinning: "fast" (default) or "off". Set by the player at
// start, read by the panel worker.
inline bool& enabled()
{
    static bool on = false;
    return on;
}

} // namespace CpuPin
