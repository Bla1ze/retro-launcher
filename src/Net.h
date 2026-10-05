#pragma once

#include <mutex>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

// Downloads, shared by Settings > Download artwork and the update check: the
// firmware's curl (as the SDK's own HTTP helper uses), a CA bundle with the
// roots the servers need, and the bits of JSON / URL handling they share.
namespace Net {

class Curl {
public:
    // Writes data/ca-bundle.pem (the firmware's bundle + our roots) and logs
    // which curl this cabinet has. Call once on the worker thread before run().
    void prepare(const std::string& appDir);
    // Runs curl with `args` (stdout to `stdoutTo`, or nowhere); its exit code,
    // -1 if it can't be started, -2 if it was killed by cancel(). `failOnHttp`:
    // curl -f (an HTTP error is exit 22; over HTTP/2 some curls say 56 instead,
    // so a caller that needs the status passes false and reads %{http_code}).
    int run(std::vector<std::string> args, const std::string& stdoutTo = "", bool failOnHttp = true);
    void cancel();  // kills running curls; run() refuses until reset()
    void reset();

private:
    std::mutex m_mu;
    std::set<pid_t> m_children;
    bool m_cancelled = false;
    std::string m_caBundle;
};

bool isNetworkError(int rc);
std::string message(int rc, const std::string& server);  // a curl failure in words
std::string readFile(const std::string& path);
std::string urlEncode(const std::string& s);
std::string jsonString(const std::string& s, size_t& at);  // s[at] == '"'; at ends past the closing quote
std::string jsonNext(const std::string& s, const char* key, size_t& at);  // next "key": "..." from at; at = npos when none

} // namespace Net
