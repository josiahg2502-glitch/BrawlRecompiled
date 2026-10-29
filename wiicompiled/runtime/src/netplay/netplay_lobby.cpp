#include "netplay_lobby.h"

#include "hle/gecko.h"

#include "nand_path.h"
#include "netplay/net_socket.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace NetplayLobby {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr uint32_t kMagic = 0x4C50574Eu;  // "NWPL"
constexpr uint8_t kVersion = 2;  // 2: Brawl (joins carry the build)
constexpr size_t kChunk = 1024;
// ~8 MiB of (packed) save data at most: a well-played save (Subspace progress, stickers) packs to near its raw ~2 MB
// and the Vault's photos and replays in collect.vff add more. The chunk bitmap goes as a U8 count of 32-bit words.
constexpr size_t kMaxChunks = 255 * 32;
constexpr auto kTimeout = 6s;

enum Type : uint8_t {
    kDiscover = 1,
    kRoomInfo = 2,
    kJoin = 3,
    kJoinReply = 4,
    kState = 5,
    kKeepAlive = 6,
    kLeave = 7,
    kStart = 8,
    kData = 9,
    kReady = 10,
    kGo = 11,
};

// The save-related NAND files every device must start from: Brawl's save (both copies of each part) and the Wii's
// settings. Without collect.vff (the Vault's album, replays and stages) the game takes the save as broken and offers
// to make a new one, which then went back over the host's own save.
const char* const kNandFiles[] = {
    "title/00010000/52534245/data/autosv0.bin",
    "title/00010000/52534245/data/autosv1.bin",
    "title/00010000/52534245/data/advsv0.bin",
    "title/00010000/52534245/data/advsv1.bin",
    "title/00010000/52534245/data/collect.vff",
    "title/00010000/52534245/data/net0.bin",
    "title/00010000/52534245/data/net1.bin",
    "title/00010000/52534245/data/banner.bin",
    "title/00000001/00000002/data/setting.txt",
};

struct Writer {
    std::vector<uint8_t> data;
    void U8(uint8_t v) { data.push_back(v); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void Raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }
    void Str(const std::string& s) {
        const size_t n = std::min<size_t>(s.size(), 63);
        U8(static_cast<uint8_t>(n));
        Raw(s.data(), n);
    }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool bad = false;
    void Raw(void* out, size_t k) {
        if (pos + k > n) {
            bad = true;
            std::memset(out, 0, k);
            return;
        }
        std::memcpy(out, p + pos, k);
        pos += k;
    }
    uint8_t U8() { uint8_t v; Raw(&v, 1); return v; }
    uint16_t U16() { uint16_t v; Raw(&v, 2); return v; }
    uint32_t U32() { uint32_t v; Raw(&v, 4); return v; }
    uint64_t U64() { uint64_t v; Raw(&v, 8); return v; }
    std::string Str() {
        const uint8_t k = U8();
        std::string s(k, '\0');
        if (k != 0) {
            Raw(s.data(), k);
        }
        return s;
    }
};

struct HostMember {
    uint8_t slot = 0;
    std::string name;
    Netplay::Address address;
    uint32_t nonce = 0;
    Clock::time_point lastHeard{};
    uint32_t rttMs = 0;
    // Start: which chunks it has, and whether it stored everything.
    std::vector<bool> chunks;
    bool started = false;
    bool ready = false;
    Clock::time_point lastRound{};
    Clock::time_point lastStart{};
};

struct FoundRoom {
    Room room;
    std::vector<uint32_t> hostIps;  // every address the host has
    Netplay::Address address;
    Clock::time_point lastSeen{};
};

struct State {
    std::mutex mutex;
    Phase phase = Phase::Off;
    std::string message;
    Netplay::UdpSocket socket;
    std::thread thread;
    std::atomic<bool> stop{false};

    // Hosting
    uint64_t roomId = 0;
    uint8_t maxPlayers = 0;
    int saveFile = 0;
    std::vector<HostMember> members;  // excluding the host
    // Browsing / member
    std::vector<FoundRoom> rooms;
    std::vector<Netplay::Address> unicastTargets;  // known and Tailscale devices, asked one by one
    std::vector<uint32_t> ownIps;
    Netplay::Address hostAddress;
    uint64_t joinRoomId = 0;
    std::vector<uint32_t> joinHostIps;
    uint32_t nonce = 0;
    uint8_t localSlot = 0;
    std::string roomName;
    std::vector<Member> roomMembers;
    Clock::time_point lastFromHost{};
    uint32_t hostTime = 0;             // the host's clock in its last room state, echoed for the RTT
    Clock::time_point hostTimeAt{};
    bool memberReady = false;          // member: the save arrived intact and is kept
    // Start (both sides)
    Plan plan;
    bool planReady = false;
    std::vector<uint8_t> blob;          // host: every NAND file back to back; member: being received
    std::vector<std::pair<std::string, uint32_t>> manifest;  // path, size
    uint64_t blobHash = 0;
    std::vector<bool> received;
    Clock::time_point startedAt{};
    Clock::time_point goAt{};
    int goSent = 0;
};

State& S() {
    static State state;
    return state;
}

uint32_t NowMs() {
    static const auto origin = Clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count());
}

std::vector<uint8_t> Header(Type type, uint64_t roomId) {
    Writer w;
    w.U32(kMagic);
    w.U8(kVersion);
    w.U8(type);
    w.U16(0);
    w.U64(roomId);
    return w.data;
}

// NSMBW_NET_TAILSCALE_ONLY=1 (Android: debug.nsmbw.tsonly): no broadcasts, and anything not from a
// Tailscale address is ignored, as if the devices were on different networks. For testing.
// BRAWL_ONLINE_ONLY=1 (testing two games on one PC): only packets through the online server count.
bool OnlineOnly() {
    static const bool only = [] {
        const char* value = std::getenv("BRAWL_ONLINE_ONLY");
        return value != nullptr && std::string(value) == "1";
    }();
    return only;
}

bool TailscaleOnly() {
    static const bool only = [] {
        const char* value = std::getenv("NSMBW_NET_TAILSCALE_ONLY");
        return value != nullptr && std::string(value) == "1";
    }();
    return only;
}

void Send(State& s, const Netplay::Address& to, Type type, uint64_t roomId, const std::vector<uint8_t>& body = {}) {
    if (TailscaleOnly() && !Netplay::IsTailscaleIp(to.ip)) {
        return;
    }
    std::vector<uint8_t> packet = Header(type, roomId);
    packet.insert(packet.end(), body.begin(), body.end());
    s.socket.SendTo(to, packet.data(), packet.size());
}

std::filesystem::path HostsFile() {
    return RuntimeConfigFile::ApplicationDataDirectory() / "Netplay" / "hosts.txt";
}

constexpr size_t kMaxKnownDevices = 64;
constexpr size_t kMaxShared = 16;  // known Tailscale devices passed on in one packet

// Hosts to ask directly: Tailscale doesn't carry broadcasts, so rooms there are found by asking
// each device. hosts.txt holds one name or address per line: devices played with or met over
// Tailscale, ones other devices passed on, and ones typed in (the Android app edits it as well).
std::mutex g_hostsMutex;

std::vector<std::string> ReadHostsLocked() {
    // Read again only when the file changed: packets ask for this every second.
    static std::vector<std::string> cached;
    static std::filesystem::file_time_type cachedStamp{};
    static bool loaded = false;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(HostsFile(), ec);
    if (loaded && !ec && stamp == cachedStamp) {
        return cached;
    }
    std::vector<std::string> hosts;
    std::ifstream in(HostsFile());
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            hosts.push_back(line);
        }
    }
    cached = hosts;
    cachedStamp = ec ? std::filesystem::file_time_type{} : stamp;
    loaded = !ec;
    return hosts;
}

void WriteHostsLocked(const std::vector<std::string>& hosts) {
    std::error_code ec;
    std::filesystem::create_directories(HostsFile().parent_path(), ec);
    std::ofstream out(HostsFile(), std::ios::trunc);
    out << "# Devices asked for LAN play rooms directly (Tailscale doesn't carry broadcasts).\n"
           "# One Tailscale name or address per line.\n";
    for (const auto& host : hosts) {
        out << host << "\n";
    }
}

std::vector<std::string> RememberedHosts() {
    std::lock_guard<std::mutex> lock(g_hostsMutex);
    return ReadHostsLocked();
}

bool RememberHostText(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_hostsMutex);
    auto hosts = ReadHostsLocked();
    if (std::find(hosts.begin(), hosts.end(), text) != hosts.end() || hosts.size() >= kMaxKnownDevices) {
        return false;
    }
    hosts.push_back(text);
    WriteHostsLocked(hosts);
    return true;
}

void RememberHost(const Netplay::Address& address) {
    RememberHostText(Netplay::IpToString(address.ip));
}

// The known devices' Tailscale addresses, passed on so that every device on the tailnet that runs
// the game learns the others through whichever one it meets.
void WriteSharedDevices(Writer& w) {
    std::vector<uint32_t> ips;
    for (const auto& host : RememberedHosts()) {
        Netplay::Address address;
        if (Netplay::ParseAddress(host, Netplay::kDefaultPort, address) && Netplay::IsTailscaleIp(address.ip)) {
            ips.push_back(address.ip);
            if (ips.size() == kMaxShared) {
                break;
            }
        }
    }
    w.U8(static_cast<uint8_t>(ips.size()));
    for (const uint32_t ip : ips) {
        w.U32(ip);
    }
    static size_t lastLogged = SIZE_MAX;
    if (ips.size() != lastLogged) {
        lastLogged = ips.size();
        RT_LOGF(RT_TAG_RUNTIME, "netplay: passing on %zu known Tailscale device(s)\n", ips.size());
    }
}

#if defined(_WIN32)
// `tailscale status --json` from the default install, else from PATH; "" without Tailscale.
std::string TailscaleStatusJson(const wchar_t* commandLine) {
    std::wstring cmd = commandLine;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return {};
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return {};
    }
    std::string json;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        json.append(buffer, got);
    }
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    return json;
}

// The Tailscale CLI's peer list: every IPv4 address under "TailscaleIPs" except our own.
std::vector<std::string> TailscalePeers() {
    std::vector<std::string> peers;
    std::string json = TailscaleStatusJson(L"\"C:\\Program Files\\Tailscale\\tailscale.exe\" status --json");
    if (json.find("\"Self\"") == std::string::npos) {
        json = TailscaleStatusJson(L"tailscale.exe status --json");
    }
    // Skip "Self" (our own entry), then collect the peers' addresses.
    const size_t self = json.find("\"Self\"");
    const size_t selfEnd = self == std::string::npos ? std::string::npos : json.find('}', json.find("\"TailscaleIPs\"", self));
    for (size_t pos = json.find("\"TailscaleIPs\""); pos != std::string::npos; pos = json.find("\"TailscaleIPs\"", pos + 1)) {
        if (self != std::string::npos && pos > self && pos < selfEnd) {
            continue;
        }
        const size_t open = json.find('[', pos);
        const size_t close = json.find(']', open);
        if (open == std::string::npos || close == std::string::npos) {
            break;
        }
        const std::string list = json.substr(open + 1, close - open - 1);
        for (size_t q = list.find('"'); q != std::string::npos; q = list.find('"', q + 1)) {
            const size_t end = list.find('"', q + 1);
            if (end == std::string::npos) {
                break;
            }
            const std::string ip = list.substr(q + 1, end - q - 1);
            Netplay::Address address;
            if (Netplay::ParseAddress(ip, Netplay::kDefaultPort, address)) {
                peers.push_back(ip);
            }
            q = end;
        }
    }
    return peers;
}
#else
std::vector<std::string> TailscalePeers() {
    return {};
}
#endif

// Brawl's save is ~2 MB, nearly all zeros: runs of zeros go as {0, length}, the rest as {1, length, bytes}.
std::vector<uint8_t> PackZeros(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    auto put = [&](uint8_t type, size_t length, const uint8_t* bytes) {
        out.push_back(type);
        const uint32_t n = static_cast<uint32_t>(length);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
        if (bytes != nullptr) out.insert(out.end(), bytes, bytes + length);
    };
    size_t literal = 0, i = 0;
    while (i < raw.size()) {
        size_t zeros = 0;
        while (i + zeros < raw.size() && raw[i + zeros] == 0) ++zeros;
        if (zeros >= 16) {
            if (i > literal) put(1, i - literal, raw.data() + literal);
            put(0, zeros, nullptr);
            i += zeros;
            literal = i;
        } else {
            i += std::max<size_t>(zeros, 1);
        }
    }
    if (raw.size() > literal) put(1, raw.size() - literal, raw.data() + literal);
    return out;
}

bool UnpackZeros(const std::vector<uint8_t>& packed, std::vector<uint8_t>& raw) {
    raw.clear();
    for (size_t pos = 0; pos < packed.size();) {
        if (pos + 5 > packed.size()) return false;
        const uint8_t type = packed[pos];
        uint32_t n = 0;
        std::memcpy(&n, packed.data() + pos + 1, 4);
        pos += 5;
        if (type == 0 && n <= 64u * 1024 * 1024) {
            raw.insert(raw.end(), n, 0);
        } else if (type == 1 && n <= packed.size() - pos) {
            raw.insert(raw.end(), packed.begin() + pos, packed.begin() + pos + n);
            pos += n;
        } else {
            return false;
        }
    }
    return true;
}

// `blob` is the files back to back, packed (PackZeros).
bool LoadNandFiles(std::vector<uint8_t>& blob, std::vector<std::pair<std::string, uint32_t>>& manifest) {
    const auto root = RuntimeNandPath::DiscoverNandRootPath();
    blob.clear();
    manifest.clear();
    for (const char* path : kNandFiles) {
        std::ifstream in(root / std::filesystem::u8path(path), std::ios::binary);
        if (!in) {
            continue;  // missing on the host: missing everywhere
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        manifest.emplace_back(path, static_cast<uint32_t>(bytes.size()));
        blob.insert(blob.end(), bytes.begin(), bytes.end());
    }
    std::vector<uint8_t> packed = PackZeros(blob), check;
    if (!UnpackZeros(packed, check) || check != blob) {  // self-check of the packer
        RT_LOGF(RT_TAG_RUNTIME, "netplay: packing the save failed its check\n");
        return false;
    }
    blob = std::move(packed);
    return blob.size() <= kMaxChunks * kChunk;
}

bool FillPlanFiles(Plan& plan, const std::vector<uint8_t>& packed, const std::vector<std::pair<std::string, uint32_t>>& manifest) {
    std::vector<uint8_t> blob;
    size_t total = 0;
    for (const auto& entry : manifest) total += entry.second;
    if (!UnpackZeros(packed, blob) || blob.size() != total) return false;
    size_t offset = 0;
    for (const auto& [path, size] : manifest) {
        plan.nandFiles[path] = std::vector<uint8_t>(blob.begin() + offset, blob.begin() + offset + size);
        offset += size;
    }
    return true;
}

std::vector<Member> HostMembersLocked(const State& s) {
    std::vector<Member> list{{0, Netplay::DeviceName()}};
    for (const auto& m : s.members) {
        list.push_back({m.slot, m.name});
    }
    std::sort(list.begin(), list.end(), [](const Member& a, const Member& b) { return a.slot < b.slot; });
    return list;
}

std::vector<uint8_t> StateBody(const State& s) {
    Writer w;
    w.U8(s.maxPlayers);
    const auto list = HostMembersLocked(s);
    w.U8(static_cast<uint8_t>(list.size()));
    for (const auto& m : list) {
        w.U8(m.slot);
        w.Str(m.name);
    }
    w.Str(s.roomName);
    w.U32(NowMs());
    return w.data;
}

std::vector<uint8_t> StartBody(const State& s, const HostMember& member) {
    Writer w;
    w.U64(s.plan.sessionId);
    w.U8(s.plan.players);
    w.U8(member.slot);
    w.U8(static_cast<uint8_t>(s.plan.inputDelay));
    w.U8(static_cast<uint8_t>(s.plan.saveFile));
    w.U8(static_cast<uint8_t>(s.plan.names.size()));
    for (const auto& name : s.plan.names) {
        w.Str(name);
    }
    w.U8(static_cast<uint8_t>(s.manifest.size()));
    for (const auto& [path, size] : s.manifest) {
        w.Str(path);
        w.U32(size);
    }
    w.U64(s.blobHash);
    w.U32(static_cast<uint32_t>(s.blob.size()));  // the files packed (PackZeros): what is sent
    return w.data;
}

// A device that runs the game, met over Tailscale or passed on by one: ask it for rooms (and tell
// it about ours) from now on, this time and next time.
void LearnLocked(State& s, uint32_t ip) {
    if (!Netplay::IsTailscaleIp(ip) || std::find(s.ownIps.begin(), s.ownIps.end(), ip) != s.ownIps.end()) {
        return;
    }
    const Netplay::Address address{ip, Netplay::kDefaultPort};
    if (std::find(s.unicastTargets.begin(), s.unicastTargets.end(), address) == s.unicastTargets.end()) {
        s.unicastTargets.push_back(address);
    }
    if (RememberHostText(Netplay::IpToString(ip))) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay: will ask %s for rooms from now on\n", Netplay::IpToString(ip).c_str());
    }
}

// The devices another one passed on (WriteSharedDevices); older builds send none.
void ReadSharedLocked(State& s, Reader& r) {
    const uint8_t count = r.U8();
    for (uint8_t i = 0; i < count && i < kMaxShared; ++i) {
        const uint32_t ip = r.U32();
        if (r.bad) {
            return;
        }
        LearnLocked(s, ip);
    }
}

std::vector<uint8_t> RoomInfoBody(const State& s) {
    Writer w;
    w.Str(s.roomName);
    w.Str(Netplay::DeviceName());
    w.U8(s.maxPlayers);
    w.U8(static_cast<uint8_t>(1 + s.members.size()));
    w.U8(s.phase == Phase::Hosting ? 1 : 0);
    // Every address this device has (LAN and Tailscale): whoever joins remembers them all, so
    // the room is found again from elsewhere, where broadcasts don't reach.
    w.U8(static_cast<uint8_t>(std::min<size_t>(s.ownIps.size(), 8)));
    for (size_t i = 0; i < s.ownIps.size() && i < 8; ++i) {
        w.U32(s.ownIps[i]);
    }
    WriteSharedDevices(w);
    return w.data;
}

// ---------------------------------------------------------------- host
void HostPacketLocked(State& s, const Netplay::Address& from, uint8_t type, uint64_t roomId, Reader& r) {
    if (type == kDiscover) {
        LearnLocked(s, from.ip);  // someone looking for rooms over Tailscale runs the game too
        ReadSharedLocked(s, r);
        Send(s, from, kRoomInfo, s.roomId, RoomInfoBody(s));
        return;
    }
    if (roomId != s.roomId) {
        return;
    }
    auto member = std::find_if(s.members.begin(), s.members.end(), [&](const HostMember& m) { return m.address == from; });
    if (type == kJoin) {
        const std::string name = r.Str();
        const uint32_t nonce = r.U32();
        const std::string build = r.Str();
        Writer reply;
        if (member == s.members.end()) {
            // Everyone must run the same game: vanilla, or the same mod's codes (Gecko::BuildIdentity).
            const bool sameBuild = !r.bad && build == Gecko::BuildIdentity();
            if (s.phase != Phase::Hosting || 1 + s.members.size() >= s.maxPlayers || !sameBuild) {
                reply.U8(!sameBuild ? 3 : s.phase != Phase::Hosting ? 2 : 1);  // other build / started / full
                reply.U8(0);
                reply.U32(nonce);
                Send(s, from, kJoinReply, s.roomId, reply.data);
                return;
            }
            uint8_t slot = 1;
            while (std::any_of(s.members.begin(), s.members.end(), [&](const HostMember& m) { return m.slot == slot; })) {
                ++slot;
            }
            HostMember added;
            added.slot = slot;
            added.name = name.empty() ? "Player" : name;
            added.address = from;
            added.nonce = nonce;
            added.lastHeard = Clock::now();
            s.members.push_back(added);
            member = std::prev(s.members.end());
            RT_LOGF(RT_TAG_RUNTIME, "netplay: %s joined the room as player %u (%s)\n", added.name.c_str(), slot + 1,
                    Netplay::ToString(from).c_str());
            RememberHost(from);  // so this device also finds that one's rooms later
        }
        reply.U8(0);
        reply.U8(member->slot);
        reply.U32(nonce);
        Send(s, from, kJoinReply, s.roomId, reply.data);
        for (const auto& m : s.members) {
            Send(s, m.address, kState, s.roomId, StateBody(s));
        }
        return;
    }
    if (member == s.members.end()) {
        return;
    }
    member->lastHeard = Clock::now();
    if (type == kLeave) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay: %s left the room\n", member->name.c_str());
        s.members.erase(member);
        for (const auto& m : s.members) {
            Send(s, m.address, kState, s.roomId, StateBody(s));
        }
        return;
    }
    if (type == kKeepAlive) {
        const uint32_t echo = r.U32();
        const uint32_t echoDelay = r.U32();
        const uint8_t phase = r.U8();
        const uint32_t chunkWords = r.U8();
        std::vector<uint32_t> bits(chunkWords);
        for (auto& b : bits) {
            b = r.U32();
        }
        if (r.bad) {
            return;
        }
        if (echo != 0 && NowMs() >= echo + echoDelay) {
            member->rttMs = NowMs() - echo - echoDelay;
        }
        if (phase >= static_cast<uint8_t>(Phase::Starting)) {
            member->started = true;
            for (size_t i = 0; i < member->chunks.size(); ++i) {
                member->chunks[i] = i / 32 < bits.size() && (bits[i / 32] >> (i % 32)) & 1u;
            }
        }
        return;
    }
    if (type == kReady) {
        if (!member->ready) {
            RT_LOGF(RT_TAG_RUNTIME, "netplay: %s has the save and is ready\n", member->name.c_str());
        }
        member->ready = true;
    }
}

void HostTickLocked(State& s) {
    const auto now = Clock::now();
    // Drop members that went quiet (not once the session is being handed out: they restart).
    if (s.phase == Phase::Hosting) {
        const size_t before = s.members.size();
        std::erase_if(s.members, [&](const HostMember& m) { return now - m.lastHeard > kTimeout; });
        if (s.members.size() != before) {
            for (const auto& m : s.members) {
                Send(s, m.address, kState, s.roomId, StateBody(s));
            }
        }
    }
    // Announce the room to the known and Tailscale devices (and the local networks): a phone looking
    // for rooms listens on the room port, so it sees this room without knowing this device.
    static Clock::time_point lastAnnounce{};
    if (s.phase == Phase::Hosting && now - lastAnnounce > 1s) {
        lastAnnounce = now;
        const auto body = RoomInfoBody(s);
        for (const auto& target : s.unicastTargets) {
            if (std::none_of(s.members.begin(), s.members.end(), [&](const HostMember& m) { return m.address.ip == target.ip; })) {
                Send(s, target, kRoomInfo, s.roomId, body);
            }
        }
        for (const auto& itf : Netplay::LocalInterfaces()) {
            if (itf.broadcast != 0) {
                Send(s, Netplay::Address{itf.broadcast, Netplay::kDefaultPort}, kRoomInfo, s.roomId, body);
            }
        }
    }
    static Clock::time_point lastState{};
    if (now - lastState > 500ms) {
        lastState = now;
        for (const auto& m : s.members) {
            Send(s, m.address, kState, s.roomId, StateBody(s));
        }
    }
    if (s.phase == Phase::Starting) {
        bool allReady = true;
        for (auto& m : s.members) {
            if (!m.started && now - m.lastStart > 200ms) {
                m.lastStart = now;
                Send(s, m.address, kStart, s.roomId, StartBody(s, m));
            }
            // What it still lacks, at most every 120 ms (its acknowledgements come every 100 ms).
            if (!m.started || now - m.lastRound < 120ms) {
                allReady = allReady && m.ready;
                continue;
            }
            m.lastRound = now;
            // At most 256 KiB a round (~2 MB/s): a big save sent whole every round would mostly be dropped.
            for (size_t i = 0, sent = 0; i < m.chunks.size() && sent < 256; ++i) {
                if (m.chunks[i]) {
                    continue;
                }
                ++sent;
                Writer w;
                w.U32(static_cast<uint32_t>(i * kChunk));
                const size_t n = std::min(kChunk, s.blob.size() - i * kChunk);
                w.U16(static_cast<uint16_t>(n));
                w.Raw(s.blob.data() + i * kChunk, n);
                Send(s, m.address, kData, s.roomId, w.data);
            }
            allReady = allReady && m.ready;
        }
        if (allReady) {
            s.phase = Phase::Restarting;
            s.goAt = now;
            s.goSent = 0;
            RT_LOGF(RT_TAG_RUNTIME, "netplay: everyone is ready; restarting into session %llu\n",
                    static_cast<unsigned long long>(s.plan.sessionId));
        } else if (now - s.startedAt > 20s) {
            s.phase = Phase::Hosting;
            s.message = "A player didn't answer. Try starting again.";
            for (auto& m : s.members) {
                m.started = m.ready = false;
            }
        }
    }
    if (s.phase == Phase::Restarting && s.goSent < 10 && now - s.goAt >= std::chrono::milliseconds(60 * s.goSent)) {
        for (const auto& m : s.members) {
            Send(s, m.address, kGo, s.roomId);
        }
        if (++s.goSent == 10) {
            s.planReady = true;
        }
    }
}

// ---------------------------------------------------------------- browser / member
void MemberPacketLocked(State& s, const Netplay::Address& from, uint8_t type, uint64_t roomId, Reader& r) {
    if (type == kRoomInfo && s.phase == Phase::Browsing) {
        FoundRoom found;
        found.room.id = roomId;
        found.room.name = r.Str();
        found.room.host = r.Str();
        found.room.maxPlayers = r.U8();
        found.room.players = r.U8();
        const bool accepting = r.U8() != 0;
        if (r.bad) {
            return;
        }
        const uint8_t addressCount = r.U8();
        for (uint8_t i = 0; i < addressCount && !r.bad; ++i) {
            found.hostIps.push_back(r.U32());
        }
        if (r.bad) {
            return;
        }
        // The host's own Tailscale address (also when it was found on the Wi-Fi) and the devices
        // it knows: next time they are asked directly, from anywhere on the tailnet.
        LearnLocked(s, from.ip);
        for (const uint32_t ip : found.hostIps) {
            LearnLocked(s, ip);
        }
        ReadSharedLocked(s, r);
        found.room.full = !accepting || found.room.players >= found.room.maxPlayers;
        found.room.address = Netplay::ToString(from);
        found.room.tailscale = Netplay::IsTailscaleIp(from.ip);
        found.address = from;
        found.lastSeen = Clock::now();
        // One entry per room; prefer the LAN address over a Tailscale one for the same room.
        auto existing = std::find_if(s.rooms.begin(), s.rooms.end(), [&](const FoundRoom& f) { return f.room.id == roomId; });
        if (existing == s.rooms.end()) {
            s.rooms.push_back(found);
        } else if (!found.room.tailscale || existing->room.tailscale) {
            *existing = found;
        } else {
            existing->lastSeen = found.lastSeen;
        }
        return;
    }
    if (from != s.hostAddress || roomId != s.joinRoomId) {
        return;
    }
    s.lastFromHost = Clock::now();
    if (type == kJoinReply && s.phase == Phase::Joining) {
        const uint8_t result = r.U8();
        const uint8_t slot = r.U8();
        if (r.bad) {
            return;
        }
        if (result != 0) {
            s.phase = Phase::Failed;
            s.message = result == 1   ? "That room is full."
                        : result == 3 ? "That room plays a different build. Pick the same one (vanilla or the same mod) "
                                        "in the launcher's Mods tab."
                                      : "That room has already started.";
            return;
        }
        s.localSlot = slot;
        s.phase = Phase::InRoom;
        // The host as reached, and its Tailscale addresses (its Wi-Fi ones are found by broadcast).
        RememberHost(from);
        for (const uint32_t ip : s.joinHostIps) {
            LearnLocked(s, ip);
        }
        RT_LOGF(RT_TAG_RUNTIME, "netplay: joined the room at %s as player %u\n", Netplay::ToString(from).c_str(), slot + 1);
        return;
    }
    if (type == kState) {
        const uint8_t maxPlayers = r.U8();
        const uint8_t count = r.U8();
        std::vector<Member> list;
        for (uint8_t i = 0; i < count; ++i) {
            Member m;
            m.slot = r.U8();
            m.name = r.Str();
            list.push_back(m);
        }
        const std::string name = r.Str();
        const uint32_t hostTime = r.U32();
        if (r.bad) {
            return;
        }
        s.hostTime = hostTime;
        s.hostTimeAt = Clock::now();
        s.maxPlayers = maxPlayers;
        s.roomMembers = list;
        s.roomName = name;
        if (s.phase == Phase::InRoom &&
            std::none_of(list.begin(), list.end(), [&](const Member& m) { return m.slot == s.localSlot; })) {
            s.phase = Phase::Failed;
            s.message = "You are no longer in the room.";
        }
        return;
    }
    if (type == kLeave) {
        s.phase = Phase::Failed;
        s.message = "The host closed the room.";
        return;
    }
    if (type == kStart && (s.phase == Phase::InRoom || s.phase == Phase::Starting)) {
        if (s.phase == Phase::Starting) {
            return;
        }
        Plan plan;
        plan.sessionId = r.U64();
        plan.players = r.U8();
        plan.localSlot = r.U8();
        plan.inputDelay = r.U8();
        plan.saveFile = r.U8();
        const uint8_t names = r.U8();
        for (uint8_t i = 0; i < names; ++i) {
            plan.names.push_back(r.Str());
        }
        const uint8_t files = r.U8();
        std::vector<std::pair<std::string, uint32_t>> manifest;
        uint32_t total = 0;
        for (uint8_t i = 0; i < files; ++i) {
            const std::string path = r.Str();
            const uint32_t size = r.U32();
            manifest.emplace_back(path, size);
            total += size;
        }
        const uint64_t hash = r.U64();
        const uint32_t packed = r.U32();
        if (r.bad || packed > kMaxChunks * kChunk || total > 64u * 1024 * 1024) {
            return;
        }
        plan.host = Netplay::ToString(s.hostAddress);
        s.plan = plan;
        s.manifest = manifest;
        s.blobHash = hash;
        s.blob.assign(packed, 0);
        s.received.assign((packed + kChunk - 1) / kChunk, false);
        s.phase = Phase::Starting;
        s.message = "Getting the host's save...";
        RT_LOGF(RT_TAG_RUNTIME, "netplay: the host started session %llu (%u players, delay %u)\n",
                static_cast<unsigned long long>(plan.sessionId), plan.players, plan.inputDelay);
        return;
    }
    if (type == kData && s.phase == Phase::Starting) {
        const uint32_t offset = r.U32();
        const uint16_t size = r.U16();
        if (r.bad || offset % kChunk != 0 || offset + size > s.blob.size() || r.pos + size > r.n) {
            return;
        }
        std::memcpy(s.blob.data() + offset, r.p + r.pos, size);
        s.received[offset / kChunk] = true;
        return;
    }
    if (type == kGo && s.phase == Phase::Starting && s.memberReady) {
        s.phase = Phase::Restarting;
        s.planReady = true;
        return;
    }
}

void MemberTickLocked(State& s) {
    const auto now = Clock::now();
    static Clock::time_point lastSend{};
    if (s.phase == Phase::Browsing) {
        static Clock::time_point lastDiscover{};
        if (now - lastDiscover > 1s) {
            lastDiscover = now;
            Writer shared;
            WriteSharedDevices(shared);
            for (const auto& itf : Netplay::LocalInterfaces()) {
                if (itf.broadcast != 0) {
                    Send(s, Netplay::Address{itf.broadcast, Netplay::kDefaultPort}, kDiscover, 0, shared.data);
                }
            }
            Send(s, Netplay::Address{0xFFFFFFFFu, Netplay::kDefaultPort}, kDiscover, 0, shared.data);
            for (const auto& target : s.unicastTargets) {
                Send(s, target, kDiscover, 0, shared.data);
            }
            // The online server's public rooms: ask each host for its room, through the server.
            Netplay::Relay::RequestList();
            for (const auto& room : Netplay::Relay::Listed()) {
                Send(s, room.host, kDiscover, 0, shared.data);
            }
        }
        std::erase_if(s.rooms, [&](const FoundRoom& f) { return now - f.lastSeen > 4s; });
        return;
    }
    if (s.phase == Phase::Joining && now - lastSend > 500ms) {
        lastSend = now;
        Writer w;
        w.Str(Netplay::DeviceName());
        w.U32(s.nonce);
        w.Str(Gecko::BuildIdentity());
        Send(s, s.hostAddress, kJoin, s.joinRoomId, w.data);
        if (now - s.startedAt > kTimeout) {
            s.phase = Phase::Failed;
            s.message = "The room didn't answer.";
        }
        return;
    }
    if (s.phase == Phase::InRoom || s.phase == Phase::Starting) {
        if (now - s.lastFromHost > kTimeout) {
            s.phase = Phase::Failed;
            s.message = "Lost the connection to the room.";
            return;
        }
        if (s.phase == Phase::Starting && !s.memberReady &&
            std::all_of(s.received.begin(), s.received.end(), [](bool b) { return b; })) {
            // Everything arrived: check it and keep it.
            if (XXH3_64bits(s.blob.data(), s.blob.size()) != s.blobHash ||
                !FillPlanFiles(s.plan, s.blob, s.manifest)) {
                std::fill(s.received.begin(), s.received.end(), false);
            } else {
                s.memberReady = true;
                s.message = "Waiting for everyone...";
            }
        }
        if (now - lastSend > (s.phase == Phase::Starting ? 100ms : 400ms)) {
            lastSend = now;
            Writer w;
            w.U32(s.hostTime);
            w.U32(s.hostTime == 0 ? 0
                                  : static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              now - s.hostTimeAt).count()));
            w.U8(static_cast<uint8_t>(s.phase));
            const size_t words = (s.received.size() + 31) / 32;
            w.U8(static_cast<uint8_t>(words));
            for (size_t i = 0; i < words; ++i) {
                uint32_t bits = 0;
                for (size_t b = 0; b < 32 && i * 32 + b < s.received.size(); ++b) {
                    bits |= (s.received[i * 32 + b] ? 1u : 0u) << b;
                }
                w.U32(bits);
            }
            Send(s, s.hostAddress, kKeepAlive, s.joinRoomId, w.data);
            if (s.memberReady) {
                Send(s, s.hostAddress, kReady, s.joinRoomId);
            }
        }
    }
}

void RefreshTargets();

void Thread() {
    State& s = S();
    std::vector<uint8_t> buffer(2048);
    while (!s.stop.load(std::memory_order_relaxed)) {
        Netplay::Address from;
        const int got = s.socket.ReceiveFrom(from, buffer.data(), buffer.size(), 5);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (got > 0 && (!TailscaleOnly() || Netplay::IsTailscaleIp(from.ip)) &&
            (!OnlineOnly() || Netplay::Relay::IsRelayAddress(from))) {
            Reader r{buffer.data(), static_cast<size_t>(got)};
            const uint32_t magic = r.U32();
            const uint8_t version = r.U8();
            const uint8_t type = r.U8();
            r.U16();
            const uint64_t roomId = r.U64();
            if (!r.bad && magic == kMagic && version == kVersion) {
                if (s.roomId != 0) {
                    HostPacketLocked(s, from, type, roomId, r);
                } else {
                    MemberPacketLocked(s, from, type, roomId, r);
                }
            }
        }
        if (s.roomId != 0) {
            HostTickLocked(s);
        } else {
            MemberTickLocked(s);
        }
        static Clock::time_point lastRefresh = Clock::now();
        if ((s.phase == Phase::Hosting || s.phase == Phase::Browsing) && Clock::now() - lastRefresh > 15s) {
            lastRefresh = Clock::now();
            RefreshTargets();
        }
    }
}

void StopLocked(State& s, std::unique_lock<std::mutex>& lock) {
    if (s.thread.joinable()) {
        // Say goodbye first, so the others don't wait out a timeout.
        if (s.roomId != 0 && s.phase != Phase::Restarting) {
            for (const auto& m : s.members) {
                Send(s, m.address, kLeave, s.roomId);
            }
        } else if (s.joinRoomId != 0 && (s.phase == Phase::InRoom || s.phase == Phase::Joining)) {
            Send(s, s.hostAddress, kLeave, s.joinRoomId);
        }
        s.stop = true;
        lock.unlock();
        s.thread.join();
        lock.lock();
    }
    s.socket.Close();
    s.stop = false;
    s.phase = Phase::Off;
    s.roomId = 0;
    s.joinRoomId = 0;
    s.members.clear();
    s.rooms.clear();
    s.roomMembers.clear();
    s.planReady = false;
    s.memberReady = false;
    s.hostTime = 0;
    s.plan = {};
    s.blob.clear();
    s.received.clear();
    s.manifest.clear();
}

// The devices to ask (and announce rooms to) one by one: the known ones plus the Tailscale CLI's
// peers. Resolving names and running the CLI can take a moment, so this runs off the game thread;
// it is repeated while the lobby is open, so devices added meanwhile are picked up.
std::atomic<bool> g_refreshing{false};

void RefreshTargets() {
    if (g_refreshing.exchange(true)) {
        return;
    }
    std::thread([] {
        std::vector<std::string> hosts = RememberedHosts();
        for (auto& peer : TailscalePeers()) {
            hosts.push_back(peer);
        }
        std::vector<Netplay::Address> targets;
        for (const auto& host : hosts) {
            Netplay::Address address;
            if (Netplay::ResolveAddress(host, Netplay::kDefaultPort, address) &&
                std::find(targets.begin(), targets.end(), address) == targets.end()) {
                targets.push_back(address);
            }
        }
        State& st = S();
        {
            std::lock_guard<std::mutex> guard(st.mutex);
            if (st.phase == Phase::Browsing || st.phase == Phase::Hosting) {
                // Keep the ones learned since (they are in hosts.txt as well, but names may not resolve).
                for (const auto& known : st.unicastTargets) {
                    if (std::find(targets.begin(), targets.end(), known) == targets.end()) {
                        targets.push_back(known);
                    }
                }
                std::erase_if(targets, [&](const Netplay::Address& a) {
                    return std::find(st.ownIps.begin(), st.ownIps.end(), a.ip) != st.ownIps.end();
                });
                if (targets.size() != st.unicastTargets.size()) {
                    RT_LOGF(RT_TAG_RUNTIME, "netplay: asking %zu known or Tailscale device(s) directly\n", targets.size());
                }
                st.unicastTargets = targets;
            }
        }
        g_refreshing = false;
    }).detach();
}

std::vector<uint32_t> OwnIps() {
    std::vector<uint32_t> ips;
    for (const auto& itf : Netplay::LocalInterfaces()) {
        if ((itf.ip >> 24) != 127) {
            ips.push_back(itf.ip);
        }
    }
    // Tailscale first: those reach a device from anywhere.
    std::stable_partition(ips.begin(), ips.end(), [](uint32_t ip) { return Netplay::IsTailscaleIp(ip); });
    return ips;
}

void NoteOwnAddresses(State& s) {
    s.ownIps = OwnIps();
}

bool OpenSocket(State& s, uint16_t port) {
    if (s.socket.Open(port)) {
        return true;
    }
    s.phase = Phase::Failed;
    s.message = port != 0 ? "Another program is using the LAN play port (UDP 52130)." : "Couldn't open a network socket.";
    return false;
}

} // namespace

Snapshot Get() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Snapshot snap;
    snap.phase = s.phase;
    snap.message = s.message;
    snap.roomName = s.roomName;
    snap.maxPlayers = s.maxPlayers;
    snap.localSlot = s.roomId != 0 ? 0 : s.localSlot;
    snap.members = s.roomId != 0 ? HostMembersLocked(s) : s.roomMembers;
    for (auto it = s.rooms.rbegin(); it != s.rooms.rend(); ++it) {
        snap.rooms.push_back(it->room);
    }
    return snap;
}

void Host(uint8_t maxPlayers, int saveFile) {
    State& s = S();
    std::unique_lock<std::mutex> lock(s.mutex);
    StopLocked(s, lock);
    std::random_device random;
    s.roomId = (static_cast<uint64_t>(random()) << 32) | random() | 1u;
    s.maxPlayers = std::clamp<uint8_t>(maxPlayers, 2, 4);
    s.saveFile = saveFile;
    s.roomName = Netplay::DeviceName() + "'s room";
    s.message.clear();
    if (!OpenSocket(s, Netplay::kDefaultPort)) {
        s.roomId = 0;
        return;
    }
    NoteOwnAddresses(s);
    s.unicastTargets.clear();
    s.phase = Phase::Hosting;
    s.thread = std::thread(Thread);
    RefreshTargets();
    RT_LOGF(RT_TAG_RUNTIME, "netplay: opened '%s' for %u players on UDP %u\n", s.roomName.c_str(), s.maxPlayers,
            s.socket.LocalPort());
}

void Browse() {
    State& s = S();
    std::unique_lock<std::mutex> lock(s.mutex);
    StopLocked(s, lock);
    s.message.clear();
#if defined(_WIN32)
    // Any port: an explicitly bound one makes Windows Firewall ask; a PC finds Tailscale rooms by
    // asking every peer `tailscale status` lists, so it needs no announcements.
    if (!OpenSocket(s, 0)) {
        return;
    }
#else
    // Listen on the room port while looking, so hosts' announcements arrive: a phone can't list the
    // tailnet's devices, but every PC and known device that hosts tells it about the room.
    if (!s.socket.Open(Netplay::kDefaultPort) && !OpenSocket(s, 0)) {
        return;
    }
#endif
    NoteOwnAddresses(s);
    s.phase = Phase::Browsing;
    s.unicastTargets.clear();
    s.thread = std::thread(Thread);
    lock.unlock();
    RefreshTargets();
}

std::vector<std::string> KnownDevices() {
    return RememberedHosts();
}

bool AddKnownDevice(const std::string& nameOrAddress) {
    std::string text = nameOrAddress;
    std::erase_if(text, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
    if (text.empty() || text.size() > 253 || text[0] == '#') {
        return false;
    }
    const bool added = RememberHostText(text);
    RefreshTargets();
    return added;
}

void RemoveKnownDevice(const std::string& nameOrAddress) {
    std::lock_guard<std::mutex> lock(g_hostsMutex);
    auto hosts = ReadHostsLocked();
    std::erase(hosts, nameOrAddress);
    WriteHostsLocked(hosts);
}

std::vector<std::string> OwnAddresses() {
    std::vector<std::string> list;
    for (const uint32_t ip : OwnIps()) {
        list.push_back(Netplay::IpToString(ip));
    }
    return list;
}

void Join(uint64_t roomId) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto it = std::find_if(s.rooms.begin(), s.rooms.end(), [&](const FoundRoom& f) { return f.room.id == roomId; });
    if (it == s.rooms.end() || s.phase != Phase::Browsing) {
        return;
    }
    s.hostAddress = it->address;
    s.joinRoomId = roomId;
    s.joinHostIps = it->hostIps;
    s.roomName = it->room.name;
    s.maxPlayers = it->room.maxPlayers;
    std::random_device random;
    s.nonce = random();
    s.phase = Phase::Joining;
    s.startedAt = Clock::now();
    s.lastFromHost = Clock::now();
    s.message.clear();
}

void Start() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.roomId == 0 || s.phase != Phase::Hosting || s.members.empty()) {
        return;
    }
    std::vector<uint8_t> blob;
    std::vector<std::pair<std::string, uint32_t>> manifest;
    if (!LoadNandFiles(blob, manifest)) {
        s.message = "The save is too big to send.";
        return;
    }
    std::random_device random;
    Plan plan;
    plan.sessionId = (static_cast<uint64_t>(random()) << 32) | random() | 1u;
    // Slots are handed out in join order; renumber so they are contiguous.
    std::sort(s.members.begin(), s.members.end(), [](const HostMember& a, const HostMember& b) { return a.slot < b.slot; });
    uint32_t worstRtt = 0;
    for (size_t i = 0; i < s.members.size(); ++i) {
        s.members[i].slot = static_cast<uint8_t>(i + 1);
        s.members[i].chunks.assign((blob.size() + kChunk - 1) / kChunk, false);
        s.members[i].started = s.members[i].ready = false;
        worstRtt = std::max(worstRtt, s.members[i].rttMs);
    }
    plan.players = static_cast<uint8_t>(1 + s.members.size());
    plan.localSlot = 0;
    // Enough frames of input delay to cover half a round trip (clients' input goes through the host).
    plan.inputDelay = std::clamp<uint32_t>((worstRtt / 2 + 12 + 16) / 17, 2, 8);
    plan.saveFile = s.saveFile;
    plan.names = {Netplay::DeviceName()};
    for (const auto& m : s.members) {
        plan.names.push_back(m.name);
    }
    FillPlanFiles(plan, blob, manifest);
    s.plan = plan;
    s.blob = std::move(blob);
    s.manifest = std::move(manifest);
    s.blobHash = XXH3_64bits(s.blob.data(), s.blob.size());
    s.phase = Phase::Starting;
    s.startedAt = Clock::now();
    s.message = "Sending your save to everyone...";
    RT_LOGF(RT_TAG_RUNTIME, "netplay: starting session %llu with %u players, input delay %u (worst RTT %u ms)\n",
            static_cast<unsigned long long>(plan.sessionId), plan.players, plan.inputDelay, worstRtt);
}

void Stop() {
    State& s = S();
    std::unique_lock<std::mutex> lock(s.mutex);
    StopLocked(s, lock);
}

bool TakePlan(Plan& out) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.planReady) {
        return false;
    }
    out = s.plan;
    return true;
}

} // namespace NetplayLobby
