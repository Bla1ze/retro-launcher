#include "Transfer.h"

#include "Library.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __APPLE__  // only for testing on a Mac (the harness ignores SIGPIPE itself)
#include <sys/mount.h>
#define SOCK_CLOEXEC 0
#define MSG_NOSIGNAL 0
#define accept4(fd, addr, len, flags) accept(fd, addr, len)
#else
#include <sys/statfs.h>
#endif
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

using Library::log;

namespace {

const int kPorts[] = {8080, 8081, 8088, 8000, 8888};
const int kMaxConnections = 6;
const int kMaxBadPins = 20;

// ------------------------------------------------------------------ the page

const char* kPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Retro Launcher transfer</title>
<style>
:root{--bg:#0e0b1f;--card:#1a1536;--line:#2c2552;--text:#eef0f8;--dim:#9aa0bd;--teal:#2bc7b8;--violet:#7c5ce8;--rose:#ef5c78;--gold:#f5d65f}
*{box-sizing:border-box}
body{margin:0;background:radial-gradient(1200px 600px at 10% -10%,#2a1a5e 0,transparent 60%),var(--bg);color:var(--text);
 font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;min-height:100vh}
main{max-width:760px;margin:0 auto;padding:24px 16px 60px}
h1{font-size:26px;margin:4px 0 2px;letter-spacing:.5px}
h1 span{color:var(--teal)}
.sub{color:var(--dim);margin:0 0 20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:18px;margin:14px 0}
label{display:block;color:var(--dim);font-size:13px;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px}
select,input{width:100%;font:inherit;color:var(--text);background:#120e28;border:1px solid var(--line);border-radius:10px;padding:12px}
input.pin{font-size:32px;letter-spacing:12px;text-align:center}
button{font:inherit;font-weight:600;border:0;border-radius:10px;padding:12px 18px;cursor:pointer;color:#0e0b1f;background:var(--teal)}
button.alt{background:#2c2552;color:var(--text)}
button:disabled{opacity:.4;cursor:default}
.row{display:flex;gap:10px;flex-wrap:wrap}
.drop{border:2px dashed #3d3470;border-radius:14px;padding:34px 16px;text-align:center;color:var(--dim);transition:.15s}
.drop.on{border-color:var(--teal);color:var(--text);background:#16123a}
.hint{color:var(--dim);font-size:14px;margin-top:10px}
.item{display:flex;align-items:center;gap:12px;padding:10px 0;border-top:1px solid var(--line)}
.item:first-child{border-top:0}
.name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.state{font-size:14px;color:var(--dim);white-space:nowrap}
.state.ok{color:var(--teal)}.state.err{color:var(--rose)}.state.skip{color:var(--gold)}
.bar{height:4px;background:#2c2552;border-radius:2px;margin-top:6px;overflow:hidden}
.bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--violet),var(--teal))}
.err{color:var(--rose)}
.hidden{display:none}
</style></head><body><main>
<h1>Retro Launcher <span>Wi-Fi transfer</span></h1>
<p class="sub">Send games from this device straight into the cabinet's library.</p>

<div id="login" class="card">
 <label for="pin">PIN shown on the cabinet</label>
 <input id="pin" class="pin" inputmode="numeric" maxlength="4" autocomplete="off" autofocus>
 <p id="loginErr" class="err hidden"></p>
 <div class="row" style="margin-top:12px"><button id="go">Connect</button></div>
</div>

<div id="app" class="hidden">
 <div class="card">
  <label for="sys">System</label>
  <select id="sys"></select>
  <p class="hint" id="exts"></p>
 </div>
 <div class="card">
  <div id="drop" class="drop">Drop games here<br><span class="hint">Disc games can come as a folder</span></div>
  <div class="row" style="margin-top:12px">
   <button id="pick">Choose files</button>
   <button id="pickDir" class="alt">Choose a folder</button>
  </div>
  <input id="files" type="file" multiple class="hidden">
  <input id="dir" type="file" webkitdirectory class="hidden">
  <p class="hint" id="space"></p>
 </div>
 <div class="card hidden" id="queueCard"><div id="queue"></div></div>
</div>
<script>
let pin = sessionStorage.getItem('pin') || '', systems = [], busy = false, queue = [];
const $ = id => document.getElementById(id);
const fmt = b => b > 1e9 ? (b / 1e9).toFixed(1) + ' GB' : b > 1e6 ? (b / 1e6).toFixed(1) + ' MB' : Math.max(1, Math.round(b / 1e3)) + ' KB';
async function api(path) {
  const r = await fetch(path, {headers: {'X-Pin': pin}, cache: 'no-store'});
  if (r.status === 403) throw new Error('pin');
  if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
  return r.json();
}
async function connect() {
  try {
    const info = await api('/api/systems');
    sessionStorage.setItem('pin', pin);
    systems = info.systems;
    const sel = $('sys'), keep = sel.value;
    sel.innerHTML = systems.map(s => `<option value="${s.id}">${s.name}  (${s.count})</option>`).join('');
    if (keep) sel.value = keep;
    showExts();
    $('space').textContent = fmt(info.free) + ' free on the USB stick' + (info.fat32 ? ' (FAT32: files up to 4 GB)' : '');
    $('login').classList.add('hidden'); $('app').classList.remove('hidden');
  } catch (e) {
    $('loginErr').textContent = e.message === 'pin' ? 'That PIN is not the one on the cabinet.' : 'Could not reach the cabinet: ' + e.message;
    $('loginErr').classList.remove('hidden');
    $('login').classList.remove('hidden'); $('app').classList.add('hidden');
  }
}
const current = () => systems.find(s => s.id === $('sys').value);
function showExts() {
  const s = current(); if (!s) return;
  $('exts').textContent = 'Accepts ' + s.exts.map(e => '.' + e).join(' ') + (s.folders ? ' (a game can be a folder)' : '');
}
// The name it gets on the stick: folders kept (one level) only where the system takes game folders.
function target(file, rel) {
  const parts = (rel || file.name).split('/').filter(p => p);
  const s = current();
  return s.folders && parts.length > 1 ? parts[parts.length - 2] + '/' + parts[parts.length - 1] : parts[parts.length - 1];
}
async function add(list) {
  const s = current(); if (!s || !list.length) return;
  let have = new Set();
  try { have = new Set((await api('/api/files?sys=' + s.id)).files); } catch (e) {}
  $('queueCard').classList.remove('hidden');
  for (const {file, rel} of list) {
    const name = target(file, rel), ext = name.split('.').pop().toLowerCase();
    const el = document.createElement('div'); el.className = 'item';
    el.innerHTML = '<div class="name"></div><div class="state"></div>';
    el.querySelector('.name').textContent = name;
    const nameBox = el.querySelector('.name'), st = el.querySelector('.state');
    $('queue').prepend(el);
    if (name.startsWith('.') || file.name.startsWith('.')) { el.remove(); continue; }
    if (!s.exts.includes(ext)) { st.textContent = 'not a ' + s.name + ' file'; st.className = 'state skip'; continue; }
    if (have.has(name)) { st.textContent = 'already there'; st.className = 'state skip'; continue; }
    const bar = document.createElement('div'); bar.className = 'bar'; bar.innerHTML = '<i></i>';
    nameBox.appendChild(bar);
    st.textContent = 'waiting';
    queue.push({file, name, sys: s.id, st, bar: bar.firstChild});
  }
  run();
}
function upload(job) {
  return new Promise(done => {
    const x = new XMLHttpRequest();
    x.open('PUT', '/api/upload?sys=' + job.sys + '&name=' + encodeURIComponent(job.name));
    x.setRequestHeader('X-Pin', pin);
    const t0 = Date.now();
    x.upload.onprogress = e => {
      if (!e.lengthComputable) return;
      job.bar.style.width = (100 * e.loaded / e.total) + '%';
      const s = (Date.now() - t0) / 1000;
      job.st.textContent = Math.round(100 * e.loaded / e.total) + '%' + (s > 1 ? '  ' + fmt(e.loaded / s) + '/s' : '');
    };
    x.onload = () => {
      let msg = ''; try { msg = JSON.parse(x.responseText).error || ''; } catch (e) {}
      if (x.status === 200) { job.st.textContent = 'sent'; job.st.className = 'state ok'; job.bar.style.width = '100%'; }
      else if (x.status === 409) { job.st.textContent = 'already there'; job.st.className = 'state skip'; }
      else { job.st.textContent = msg || ('error ' + x.status); job.st.className = 'state err'; }
      done();
    };
    x.onerror = () => { job.st.textContent = 'connection lost'; job.st.className = 'state err'; done(); };
    job.st.textContent = '0%';
    x.send(job.file);
  });
}
async function run() {
  if (busy) return; busy = true;
  while (queue.length) await upload(queue.shift());
  busy = false;
  connect();
}
// Folders dropped on the page: walk them for their files.
function walk(entry, out) {
  return new Promise(res => {
    if (entry.isFile) entry.file(f => { out.push({file: f, rel: entry.fullPath.slice(1)}); res(); }, () => res());
    else if (entry.isDirectory) {
      const r = entry.createReader(), all = [];
      const more = () => r.readEntries(async ents => {
        if (!ents.length) { for (const e of all) await walk(e, out); res(); }
        else { all.push(...ents); more(); }
      }, () => res());
      more();
    } else res();
  });
}
$('go').onclick = () => { pin = $('pin').value.trim(); connect(); };
$('pin').onkeydown = e => { if (e.key === 'Enter') $('go').click(); };
$('sys').onchange = showExts;
$('pick').onclick = () => $('files').click();
$('pickDir').onclick = () => $('dir').click();
$('files').onchange = e => { add([...e.target.files].map(f => ({file: f, rel: f.name}))); e.target.value = ''; };
$('dir').onchange = e => { add([...e.target.files].map(f => ({file: f, rel: f.webkitRelativePath}))); e.target.value = ''; };
const drop = $('drop');
['dragenter', 'dragover'].forEach(t => drop.addEventListener(t, e => { e.preventDefault(); drop.classList.add('on'); }));
['dragleave', 'drop'].forEach(t => drop.addEventListener(t, e => { e.preventDefault(); drop.classList.remove('on'); }));
drop.addEventListener('drop', async e => {
  const out = [], items = [...e.dataTransfer.items].map(i => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
  if (items.length) for (const it of items) await walk(it, out);
  else for (const f of e.dataTransfer.files) out.push({file: f, rel: f.name});
  add(out);
});
if (pin) connect();
</script></main></body></html>
)HTML";

// ------------------------------------------------------------------ helpers

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out + "\"";
}

bool sendAll(int fd, const char* data, size_t n) {
    while (n) {
        ssize_t w = ::send(fd, data, n, MSG_NOSIGNAL);  // never SIGPIPE: a closed tab must not kill the menu
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        data += w;
        n -= (size_t)w;
    }
    return true;
}

void respond(int fd, int code, const char* type, const std::string& body) {
    const char* text = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                     : code == 409 ? "Conflict" : code == 411 ? "Length Required" : code == 413 ? "Payload Too Large"
                     : code == 429 ? "Too Many Requests" : code == 507 ? "Insufficient Storage" : "Error";
    char head[256];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                  "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                  code, text, type, body.size());
    sendAll(fd, head, std::strlen(head)) && sendAll(fd, body.data(), body.size());
}

void respondError(int fd, int code, const std::string& msg) {
    respond(fd, code, "application/json", "{\"error\":" + jsonStr(msg) + "}");
}

void setTimeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool isDirPath(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Systems whose games can be folders (disc sets, NAOMI / Atomiswave GD-ROM CHDs).
bool takesFolders(const std::string& id) {
    return id == "psx" || id == "saturn" || id == "dreamcast" || id == "psp" || id == "naomi" || id == "atomiswave";
}

std::vector<std::string> allowedExts(const Library::System& s) {
    std::vector<std::string> e = s.extensions;
    e.push_back("zip");
    if (s.id == "psx" || s.id == "saturn" || s.id == "dreamcast")
        for (const char* x : {"bin", "raw", "img", "sub", "ccd", "cue", "gdi", "iso", "chd", "m3u", "cdi", "pbp"}) e.push_back(x);
    if (s.id == "naomi" || s.id == "atomiswave") e.push_back("chd");
    std::sort(e.begin(), e.end());
    e.erase(std::unique(e.begin(), e.end()), e.end());
    return e;
}

// A name the browser asked for, checked: one or two plain components, the second
// (if any) only where the system takes folders. "" if refused.
std::string safeName(const std::string& name, bool folders, std::string& why) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (true) {
        size_t slash = name.find('/', at);
        parts.push_back(name.substr(at, slash == std::string::npos ? std::string::npos : slash - at));
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    if (parts.size() > (folders ? 2u : 1u)) { why = folders ? "only one folder level" : "this system takes files, not folders"; return ""; }
    for (const std::string& p : parts) {
        if (p.empty() || p == "." || p == ".." || p[0] == '.' || p.size() > 200) { why = "bad name"; return ""; }
        for (unsigned char c : p)
            if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                why = "bad character in the name";
                return "";
            }
    }
    return name;
}

std::string extOf(const std::string& name) {
    size_t dot = name.find_last_of('.');
    size_t slash = name.find_last_of('/');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : lower(name.substr(dot + 1));
}

} // namespace

// ------------------------------------------------------------------ server

bool TransferServer::start(const std::string& appDir) {
    stop();
    m_appDir = appDir;
    m_stop = false;
    m_badPins = 0;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_status = Status();
        unsigned v = 0;
        int r = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (r >= 0) { if (::read(r, &v, sizeof(v)) != sizeof(v)) v = 0; ::close(r); }
        if (!v) v = (unsigned)::time(nullptr) ^ (unsigned)::getpid() * 2654435761u;
        char pin[8];
        std::snprintf(pin, sizeof(pin), "%04u", v % 10000);
        m_status.pin = pin;
        // The IPv4 addresses that are up. Internal ones are left off the screen:
        // link-local 169.254.x.x (an interface with no network) and the
        // firmware's container bridges (docker0, br-..., veth...). They are
        // logged, and shown if nothing else is there.
        std::vector<std::string> internal;
        struct ifaddrs* ifs = nullptr;
        if (::getifaddrs(&ifs) == 0) {
            for (struct ifaddrs* i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
                char buf[INET_ADDRSTRLEN];
                const uint32_t ip = ntohl(((struct sockaddr_in*)i->ifa_addr)->sin_addr.s_addr);
                ::inet_ntop(AF_INET, &((struct sockaddr_in*)i->ifa_addr)->sin_addr, buf, sizeof(buf));
                const std::string ifname = i->ifa_name ? i->ifa_name : "";
                const bool hidden = (ip >> 16) == 0xA9FE ||  // 169.254.0.0/16
                                    ifname.compare(0, 6, "docker") == 0 || ifname.compare(0, 3, "br-") == 0 ||
                                    ifname.compare(0, 4, "veth") == 0 || ifname.compare(0, 6, "virbr") == 0;
                (hidden ? internal : m_status.addresses).push_back(buf);
                log("transfer: %s %s%s", ifname.c_str(), buf, hidden ? " (internal, not shown)" : "");
            }
            ::freeifaddrs(ifs);
        }
        if (m_status.addresses.empty()) m_status.addresses = internal;
    }
    // Leftovers from an upload cut off by a power loss.
    for (const Library::System& s : Library::systems()) {
        const std::string dir = appDir + "/roms/" + s.id;
        if (DIR* d = ::opendir(dir.c_str())) {
            while (struct dirent* e = ::readdir(d)) {
                std::string n = e->d_name;
                if (n.compare(0, 8, ".upload-") == 0) ::unlink((dir + "/" + n).c_str());
            }
            ::closedir(d);
        }
    }

    m_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);  // CLOEXEC: curl children must not inherit it
    std::string error = m_listen < 0 ? std::string("socket: ") + std::strerror(errno) : "";
    int port = 0;
    if (m_listen >= 0) {
        int one = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        for (int p : kPorts) {
            struct sockaddr_in a;
            std::memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons((uint16_t)p);
            if (::bind(m_listen, (struct sockaddr*)&a, sizeof(a)) == 0) { port = p; break; }
            error = "port " + std::to_string(p) + ": " + std::strerror(errno);
        }
        if (port && ::listen(m_listen, 8) != 0) { error = std::string("listen: ") + std::strerror(errno); port = 0; }
        if (!port) { ::close(m_listen); m_listen = -1; }
    }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_status.listening = port != 0;
        m_status.port = port;
        m_status.error = port ? "" : error;
    }
    std::string addrs;
    for (const std::string& a : m_status.addresses) addrs += " " + a;
    if (!port) { log("transfer: not listening (%s); addresses:%s", error.c_str(), addrs.c_str()); return false; }
    log("transfer: listening on port %d; addresses:%s", port, addrs.empty() ? " (none)" : addrs.c_str());
    m_running = true;
    m_acceptThread = std::thread([this]() { acceptLoop(); });
    return true;
}

void TransferServer::stop() {
    m_stop = true;
    if (m_listen >= 0) ::shutdown(m_listen, SHUT_RDWR);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        for (int fd : m_fds) ::shutdown(fd, SHUT_RDWR);  // unblocks uploads in progress
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();
    for (std::thread& t : m_threads)
        if (t.joinable()) t.join();
    m_threads.clear();
    if (m_listen >= 0) { ::close(m_listen); m_listen = -1; }
    if (m_running) log("transfer: stopped");
    m_running = false;
}

TransferServer::Status TransferServer::status() const {
    std::lock_guard<std::mutex> lock(m_mu);
    Status s = m_status;
    s.connections = m_active.load();
    return s;
}

void TransferServer::note(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_status.recent.insert(m_status.recent.begin(), line);
    if (m_status.recent.size() > 6) m_status.recent.pop_back();
}

void TransferServer::acceptLoop() {
    while (!m_stop) {
        struct pollfd p = {m_listen, POLLIN, 0};
        if (::poll(&p, 1, 250) <= 0) continue;
        int fd = ::accept4(m_listen, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        if (m_active >= kMaxConnections) { ::close(fd); continue; }
        // Finished connection threads are joined as new ones come in.
        for (auto it = m_threads.begin(); it != m_threads.end();) {
            if (it->joinable() && m_active == 0) { it->join(); it = m_threads.erase(it); }
            else ++it;
        }
        ++m_active;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_fds.insert(fd);
        }
        m_threads.emplace_back([this, fd]() {
            serve(fd);
            {
                std::lock_guard<std::mutex> lock(m_mu);
                m_fds.erase(fd);
            }
            ::close(fd);
            --m_active;
        });
    }
}

// One request per connection (Connection: close).
void TransferServer::serve(int fd) {
    // A browser opens spare connections that may never send anything: give up
    // on those quickly; uploads then get a longer timeout per read.
    setTimeout(fd, 5);
    std::string head;
    char buf[4096];
    size_t end = std::string::npos;
    while (head.size() < 16384) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        head.append(buf, (size_t)n);
        if ((end = head.find("\r\n\r\n")) != std::string::npos) break;
    }
    if (end == std::string::npos) { respondError(fd, 400, "request too large"); return; }
    std::string body = head.substr(end + 4);  // what came with the headers
    head.resize(end);

    // Request line and headers.
    size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol), method, target;
    {
        size_t a = line.find(' '), b = line.find(' ', a + 1);
        if (a == std::string::npos || b == std::string::npos) { respondError(fd, 400, "bad request"); return; }
        method = line.substr(0, a);
        target = line.substr(a + 1, b - a - 1);
    }
    std::string pin, expect, transferEncoding;
    long long length = -1;
    for (size_t at = eol; at != std::string::npos && at < head.size();) {
        size_t next = head.find("\r\n", at + 2);
        std::string h = head.substr(at + 2, next == std::string::npos ? std::string::npos : next - at - 2);
        at = next;
        size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        std::string k = lower(h.substr(0, colon)), v = h.substr(colon + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        if (k == "x-pin") pin = v;
        else if (k == "content-length") length = (long long)std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "expect") expect = lower(v);
        else if (k == "transfer-encoding") transferEncoding = lower(v);
    }
    std::string path = target, query;
    const size_t q = target.find('?');
    if (q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }
    auto param = [&](const char* key) {
        const std::string k = std::string(key) + "=";
        for (size_t at = 0; at <= query.size();) {
            size_t amp = query.find('&', at);
            std::string kv = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
            if (kv.compare(0, k.size(), k) == 0) return urlDecode(kv.substr(k.size()));
            if (amp == std::string::npos) break;
            at = amp + 1;
        }
        return std::string();
    };
    log("transfer: %s %s", method.c_str(), path.c_str());

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", kPage);
        return;
    }
    if (path.compare(0, 5, "/api/") != 0) { respondError(fd, 404, "not found"); return; }
    if (m_badPins >= kMaxBadPins) { respondError(fd, 429, "too many wrong PINs - close and reopen Wi-Fi transfer on the cabinet"); return; }
    std::string want;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        want = m_status.pin;
    }
    if (pin != want) { ++m_badPins; respondError(fd, 403, "wrong PIN"); return; }

    const std::string roms = m_appDir + "/roms/";
    if (method == "GET" && path == "/api/systems") {
        struct statvfs vs;
        unsigned long long freeBytes = ::statvfs(m_appDir.c_str(), &vs) == 0 ? (unsigned long long)vs.f_bavail * vs.f_frsize : 0;
        struct statfs fs;
        bool fat32 = ::statfs(m_appDir.c_str(), &fs) == 0 && fs.f_type == 0x4d44;
        std::string out = "{\"free\":" + std::to_string(freeBytes) + ",\"fat32\":" + (fat32 ? "true" : "false") + ",\"systems\":[";
        bool first = true;
        for (const Library::System& s : Library::systems()) {
            int count = 0;
            if (DIR* d = ::opendir((roms + s.id).c_str())) {
                while (struct dirent* e = ::readdir(d))
                    if (e->d_name[0] != '.' && std::strcmp(e->d_name, "README.txt") != 0) ++count;
                ::closedir(d);
            }
            std::string exts;
            for (const std::string& e : allowedExts(s)) exts += (exts.empty() ? "" : ",") + jsonStr(e);
            out += std::string(first ? "" : ",") + "{\"id\":" + jsonStr(s.id) + ",\"name\":" + jsonStr(s.name) +
                   ",\"count\":" + std::to_string(count) + ",\"folders\":" + (takesFolders(s.id) ? "true" : "false") +
                   ",\"exts\":[" + exts + "]}";
            first = false;
        }
        respond(fd, 200, "application/json", out + "]}");
        return;
    }

    const std::string sysId = param("sys");
    const Library::System* sys = Library::findSystem(sysId);
    if (!sys) { respondError(fd, 400, "unknown system"); return; }
    const std::string dir = roms + sys->id;

    if (method == "GET" && path == "/api/files") {
        // Names as the page would send them: files, and folder/file one level down.
        std::string out = "{\"files\":[";
        bool first = true;
        auto add = [&](const std::string& n) { out += std::string(first ? "" : ",") + jsonStr(n); first = false; };
        if (DIR* d = ::opendir(dir.c_str())) {
            while (struct dirent* e = ::readdir(d)) {
                std::string n = e->d_name;
                if (n.empty() || n[0] == '.') continue;
                if (isDirPath(dir + "/" + n)) {
                    if (DIR* sd = ::opendir((dir + "/" + n).c_str())) {
                        while (struct dirent* se = ::readdir(sd))
                            if (se->d_name[0] != '.') add(n + "/" + se->d_name);
                        ::closedir(sd);
                    }
                } else {
                    add(n);
                }
            }
            ::closedir(d);
        }
        respond(fd, 200, "application/json", out + "]}");
        return;
    }

    if (method != "PUT" || path != "/api/upload") { respondError(fd, 404, "not found"); return; }
    if (!transferEncoding.empty() || length < 0) { respondError(fd, 411, "send the file with a Content-Length"); return; }
    std::string why;
    const std::string name = safeName(param("name"), takesFolders(sys->id), why);
    if (name.empty()) { respondError(fd, 400, why); return; }
    const std::vector<std::string> exts = allowedExts(*sys);
    const std::string ext = extOf(name);
    if (std::find(exts.begin(), exts.end(), ext) == exts.end()) { respondError(fd, 400, "not a " + sys->name + " file type"); return; }
    const std::string dest = dir + "/" + name;
    struct stat st;
    if (::stat(dest.c_str(), &st) == 0) { respondError(fd, 409, "already there"); return; }
    {
        struct statvfs vs;
        if (::statvfs(m_appDir.c_str(), &vs) == 0 &&
            (unsigned long long)length + (64ull << 20) > (unsigned long long)vs.f_bavail * vs.f_frsize) {
            respondError(fd, 507, "not enough space on the USB stick");
            return;
        }
        struct statfs fs;
        if (::statfs(m_appDir.c_str(), &fs) == 0 && fs.f_type == 0x4d44 && length > 0xFFFFFFFFLL) {
            respondError(fd, 413, "over 4 GB: the stick is FAT32 (use a .chd or .cso to make it smaller)");
            return;
        }
    }
    ::mkdir(dir.c_str(), 0755);
    std::string folder = dest.substr(0, dest.find_last_of('/'));
    if (folder != dir) ::mkdir(folder.c_str(), 0755);
    const std::string part = folder + "/.upload-" + std::to_string((long)::getpid()) + "-" + std::to_string(fd) + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) { respondError(fd, 500, std::string("can't write: ") + std::strerror(errno)); return; }
    if (expect == "100-continue") sendAll(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_status.current = sys->shortName + ": " + name;
        m_status.currentDone = 0;
        m_status.currentTotal = (uint64_t)length;
    }
    setTimeout(fd, 30);
    std::vector<char> chunk(1 << 20);
    unsigned long long got = 0;
    bool ok = true;
    if (!body.empty()) {
        size_t take = (size_t)std::min<unsigned long long>(body.size(), (unsigned long long)length);
        ok = std::fwrite(body.data(), 1, take, f) == take;
        got = take;
    }
    while (ok && got < (unsigned long long)length && !m_stop) {
        size_t want = (size_t)std::min<unsigned long long>(chunk.size(), (unsigned long long)length - got);
        ssize_t n = ::recv(fd, chunk.data(), want, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        if (std::fwrite(chunk.data(), 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
        got += (unsigned long long)n;
        std::lock_guard<std::mutex> lock(m_mu);
        m_status.currentDone = got;
    }
    ok = ok && got == (unsigned long long)length && std::fflush(f) == 0 && ::fsync(fileno(f)) == 0;
    ok = (std::fclose(f) == 0) && ok;
    if (ok && ::rename(part.c_str(), dest.c_str()) != 0) ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_status.current.clear();
        if (ok) { ++m_status.received; m_status.receivedBytes += got; }
    }
    if (!ok) {
        ::unlink(part.c_str());
        if (folder != dir) ::rmdir(folder.c_str());  // only if it is empty
        log("transfer: %s/%s failed after %llu of %lld bytes", sys->id.c_str(), name.c_str(), got, length);
        note(sys->shortName + ": " + name + " - failed");
        respondError(fd, 500, "the upload was cut off");
        return;
    }
    log("transfer: received %s/%s (%llu bytes)", sys->id.c_str(), name.c_str(), got);
    note(sys->shortName + ": " + name);
    respond(fd, 200, "application/json", "{\"ok\":true}");
}
