// FTL co-op: the real Steam behind coop::SteamApi.
// FTL itself loads Steam (libsteam_api.so / steam_api.dll, Steamworks SDK of 2019, app 212680) and calls
// SteamAPI_Init. We look up the library the game already loaded and call its flat C functions, which take the
// interface pointer first. No SDK headers, nothing linked. Callbacks are not used: asynchronous results are polled
// with ISteamUtils::GetAPICallResult, membership with GetNumLobbyMembers, packets with IsP2PPacketAvailable.
// The vtable slots behind every flat function used here were checked in the Linux library (CreateLobby 13,
// JoinLobby 14, GetLobbyMemberByIndex 18, GetLobbyOwner 35, SendP2PPacket 0, ReadP2PPacket 2, Accept 3, ...), and
// match the interface versions requested below.
#include "CoopSteam.h"
#include "CoopGame.h"

#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace CoopSteam {

typedef uint64_t SteamID;
typedef uint64_t ApiCall;

// Steam's callback structs: 4-byte packing on Linux and macOS, 8-byte on Windows (VALVE_CALLBACK_PACK_*).
#ifdef _WIN32
#pragma pack(push, 8)
#else
#pragma pack(push, 4)
#endif
struct LobbyCreated { int32_t result; uint64_t lobby; };                                          // 513
struct LobbyEnter { uint64_t lobby; uint32_t perms; bool locked; uint32_t response; };           // 504
#pragma pack(pop)
struct FriendGameInfo { uint64_t gameId; uint32_t ip; uint16_t port, queryPort; uint64_t lobby; }; // 24 bytes

static const uint32_t kFtlAppId = 212680;

struct Fns
{
    int (*GetHSteamUser)();
    int (*GetHSteamPipe)();
    void* (*FindOrCreateUserInterface)(int user, const char* version);
    void* (*Client)();
    void* (*ClientGetUser)(void*, int, int, const char*);
    void* (*ClientGetFriends)(void*, int, int, const char*);
    void* (*ClientGetUtils)(void*, int, const char*);
    void* (*ClientGetMatchmaking)(void*, int, int, const char*);
    void* (*ClientGetNetworking)(void*, int, int, const char*);
    SteamID (*UserGetSteamID)(void*);
    const char* (*GetPersonaName)(void*);
    int (*GetFriendCount)(void*, int flags);
    SteamID (*GetFriendByIndex)(void*, int i, int flags);
    const char* (*GetFriendPersonaName)(void*, SteamID);
    bool (*GetFriendGamePlayed)(void*, SteamID, FriendGameInfo*);
    bool (*IsAPICallCompleted)(void*, ApiCall, bool* failed);
    bool (*GetAPICallResult)(void*, ApiCall, void* out, int size, int callbackId, bool* failed);
    ApiCall (*CreateLobby)(void*, int type, int maxMembers);
    ApiCall (*JoinLobby)(void*, SteamID);
    void (*LeaveLobby)(void*, SteamID);
    int (*GetNumLobbyMembers)(void*, SteamID);
    SteamID (*GetLobbyMemberByIndex)(void*, SteamID, int);
    const char* (*GetLobbyData)(void*, SteamID, const char*);
    bool (*SetLobbyData)(void*, SteamID, const char*, const char*);
    SteamID (*GetLobbyOwner)(void*, SteamID);
    bool (*SendP2PPacket)(void*, SteamID, const void*, uint32_t, int sendType, int channel);
    bool (*IsP2PPacketAvailable)(void*, uint32_t* size, int channel);
    bool (*ReadP2PPacket)(void*, void*, uint32_t, uint32_t* size, SteamID* from, int channel);
    bool (*AcceptP2PSessionWithUser)(void*, SteamID);
    bool (*CloseP2PSessionWithUser)(void*, SteamID);
    bool (*AllowP2PPacketRelay)(void*, bool);
};

static void* Sym(const char* name)
{
#ifdef _WIN32
    // Windows FTL 1.6.9 (the version Hyperspace needs) has no Steam code: it never loads steam_api.dll, so we do.
    // The DLL comes with the Steam install of FTL, next to FTLGame.exe.
    static HMODULE m = nullptr;
    if (!m) m = GetModuleHandleA("steam_api.dll");
    if (!m) m = LoadLibraryA("steam_api.dll");
    return m ? (void*)GetProcAddress(m, name) : nullptr;
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

class RealSteam : public coop::SteamApi
{
public:
    bool Load(std::string* err)
    {
        std::string missing;
#define LOAD(field, name) do { *(void**)&f.field = Sym(name); if (!f.field) missing += std::string(" ") + name; } while (0)
        LOAD(GetHSteamUser, "SteamAPI_GetHSteamUser");
        LOAD(GetHSteamPipe, "SteamAPI_GetHSteamPipe");
        LOAD(FindOrCreateUserInterface, "SteamInternal_FindOrCreateUserInterface");
        LOAD(Client, "SteamClient");
        LOAD(ClientGetUser, "SteamAPI_ISteamClient_GetISteamUser");
        LOAD(ClientGetFriends, "SteamAPI_ISteamClient_GetISteamFriends");
        LOAD(ClientGetUtils, "SteamAPI_ISteamClient_GetISteamUtils");
        LOAD(ClientGetMatchmaking, "SteamAPI_ISteamClient_GetISteamMatchmaking");
        LOAD(ClientGetNetworking, "SteamAPI_ISteamClient_GetISteamNetworking");
        LOAD(UserGetSteamID, "SteamAPI_ISteamUser_GetSteamID");
        LOAD(GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
        LOAD(GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
        LOAD(GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
        LOAD(GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
        LOAD(GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
        LOAD(IsAPICallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted");
        LOAD(GetAPICallResult, "SteamAPI_ISteamUtils_GetAPICallResult");
        LOAD(CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
        LOAD(JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
        LOAD(LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
        LOAD(GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
        LOAD(GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
        LOAD(GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
        LOAD(SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
        LOAD(GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
        LOAD(SendP2PPacket, "SteamAPI_ISteamNetworking_SendP2PPacket");
        LOAD(IsP2PPacketAvailable, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
        LOAD(ReadP2PPacket, "SteamAPI_ISteamNetworking_ReadP2PPacket");
        LOAD(AcceptP2PSessionWithUser, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
        LOAD(CloseP2PSessionWithUser, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
        LOAD(AllowP2PPacketRelay, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
#undef LOAD
        if (!missing.empty())
        {
            *err = Sym("SteamAPI_Init") ? "FTL's Steam library lacks:" + missing : "steam_api.dll is not in the FTL folder";
            return false;
        }
        int hUser = f.GetHSteamUser(), hPipe = f.GetHSteamPipe();
        if (!hUser || !hPipe)
        {
            // The game did not start Steam (Windows 1.6.9): start it ourselves for app 212680 and pump its callbacks.
            bool (*init)() = nullptr;
            *(void**)&init = Sym("SteamAPI_Init");
            *(void**)&m_runCallbacks = Sym("SteamAPI_RunCallbacks");
#ifdef _WIN32
            if (!getenv("SteamAppId")) SetEnvironmentVariableA("SteamAppId", "212680");
            if (!getenv("SteamGameId")) SetEnvironmentVariableA("SteamGameId", "212680");
#endif
            bool ok = init && init();
            CoopGame::Logf("Steam: the game had not started Steam; SteamAPI_Init %s", ok ? "ok" : "failed");
            if (!ok) { *err = "could not start Steam (is Steam running and signed in?)"; return false; }
            hUser = f.GetHSteamUser(); hPipe = f.GetHSteamPipe();
        }
        if (!hUser || !hPipe) { *err = "Steam is not running for this game (start Steam, then FTL)"; return false; }
        void* client = f.Client();
        auto get = [&](const char* ver, int kind) -> void* {
            void* p = f.FindOrCreateUserInterface(hUser, ver);
            if (p || !client) return p;
            switch (kind)
            {
            case 0: return f.ClientGetUser(client, hUser, hPipe, ver);
            case 1: return f.ClientGetFriends(client, hUser, hPipe, ver);
            case 2: return f.ClientGetUtils(client, hPipe, ver);
            case 3: return f.ClientGetMatchmaking(client, hUser, hPipe, ver);
            default: return f.ClientGetNetworking(client, hUser, hPipe, ver);
            }
        };
        m_user = get("SteamUser020", 0);
        m_friends = get("SteamFriends017", 1);
        m_utils = get("SteamUtils009", 2);
        m_mm = get("SteamMatchMaking009", 3);
        m_net = get("SteamNetworking005", 4);
        if (!m_user || !m_friends || !m_utils || !m_mm || !m_net)
        {
            *err = "Steam interfaces missing (user " + std::to_string(m_user != nullptr) + " friends " + std::to_string(m_friends != nullptr) +
                   " utils " + std::to_string(m_utils != nullptr) + " matchmaking " + std::to_string(m_mm != nullptr) +
                   " networking " + std::to_string(m_net != nullptr) + ")";
            return false;
        }
        m_me = f.UserGetSteamID(m_user);
        if (!m_me) { *err = "not logged in to Steam"; return false; }
        f.AllowP2PPacketRelay(m_net, true);
        return true;
    }

    void Tick() override { if (m_runCallbacks) m_runCallbacks(); }
    uint64_t Me() override { return m_me; }
    std::string Name(uint64_t u) override
    {
        const char* n = u == m_me ? f.GetPersonaName(m_friends) : f.GetFriendPersonaName(m_friends, u);
        return n && *n ? n : std::to_string(u);
    }
    uint64_t CreateLobby(int maxMembers) override { return f.CreateLobby(m_mm, 1 /* friends only */, maxMembers); }
    uint64_t JoinLobby(uint64_t lobby) override { return f.JoinLobby(m_mm, lobby); }
    int CallResult(uint64_t call, bool isCreate, uint64_t* lobby) override
    {
        bool failed = false;
        if (!f.IsAPICallCompleted(m_utils, call, &failed)) return failed ? -1 : 0;
        if (isCreate)
        {
            LobbyCreated r; memset(&r, 0, sizeof(r));
            if (!f.GetAPICallResult(m_utils, call, &r, (int)sizeof(r), 513, &failed) || failed)
            { CoopGame::Logf("Steam: LobbyCreated result unreadable (size %d)", (int)sizeof(r)); return -1; }
            CoopGame::Logf("Steam: LobbyCreated result %d lobby %llu", r.result, (unsigned long long)r.lobby);
            *lobby = r.lobby;
            return r.result == 1 && r.lobby ? 1 : -1;
        }
        LobbyEnter r; memset(&r, 0, sizeof(r));
        if (!f.GetAPICallResult(m_utils, call, &r, (int)sizeof(r), 504, &failed) || failed)
        { CoopGame::Logf("Steam: LobbyEnter result unreadable (size %d)", (int)sizeof(r)); return -1; }
        CoopGame::Logf("Steam: LobbyEnter response %u lobby %llu", r.response, (unsigned long long)r.lobby);
        *lobby = r.lobby;
        return r.response == 1 ? 1 : -1;   // k_EChatRoomEnterResponseSuccess
    }
    void LeaveLobby(uint64_t lobby) override { f.LeaveLobby(m_mm, lobby); }
    std::vector<uint64_t> Members(uint64_t lobby) override
    {
        std::vector<uint64_t> v;
        int n = f.GetNumLobbyMembers(m_mm, lobby);
        for (int i = 0; i < n && i < 64; ++i) v.push_back(f.GetLobbyMemberByIndex(m_mm, lobby, i));
        return v;
    }
    uint64_t Owner(uint64_t lobby) override { return f.GetLobbyOwner(m_mm, lobby); }
    void SetData(uint64_t lobby, const std::string& k, const std::string& v) override { f.SetLobbyData(m_mm, lobby, k.c_str(), v.c_str()); }
    std::string GetData(uint64_t lobby, const std::string& k) override { const char* s = f.GetLobbyData(m_mm, lobby, k.c_str()); return s ? s : ""; }
    std::vector<FriendLobby> FriendLobbies() override
    {
        std::vector<FriendLobby> v;
        const int kImmediate = 4;
        int n = f.GetFriendCount(m_friends, kImmediate);
        for (int i = 0; i < n && i < 2000; ++i)
        {
            SteamID fr = f.GetFriendByIndex(m_friends, i, kImmediate);
            FriendGameInfo g; memset(&g, 0, sizeof(g));
            if (!f.GetFriendGamePlayed(m_friends, fr, &g)) continue;
            if ((uint32_t)(g.gameId & 0xffffff) != kFtlAppId || !g.lobby) continue;
            v.push_back(FriendLobby{ fr, g.lobby, Name(fr) });
        }
        static bool logged = false;
        if (!logged) { logged = true; CoopGame::Logf("Steam: friends list has %d friends, %zu in an FTL lobby", n, v.size()); }
        return v;
    }
    bool Send(uint64_t user, const uint8_t* d, uint32_t n) override { return f.SendP2PPacket(m_net, user, d, n, 2 /* reliable */, 0); }
    bool Receive(coop::Bytes* out, uint64_t* from) override
    {
        uint32_t size = 0;
        if (!f.IsP2PPacketAvailable(m_net, &size, 0)) return false;
        out->resize(size ? size : 1);
        SteamID who = 0; uint32_t got = 0;
        if (!f.ReadP2PPacket(m_net, out->data(), (uint32_t)out->size(), &got, &who, 0)) return false;
        out->resize(got);
        *from = who;
        return true;
    }
    void Accept(uint64_t user) override { f.AcceptP2PSessionWithUser(m_net, user); }
    void Close(uint64_t user) override { f.CloseP2PSessionWithUser(m_net, user); }

private:
    Fns f = {};
    void (*m_runCallbacks)() = nullptr;   // set when we started Steam ourselves
    void *m_user = nullptr, *m_friends = nullptr, *m_utils = nullptr, *m_mm = nullptr, *m_net = nullptr;
    uint64_t m_me = 0;
};

coop::SteamApi* Open(std::string* err)
{
    static RealSteam* s = nullptr;
    if (s) return s;
    RealSteam* r = new RealSteam();
    if (!r->Load(err)) { delete r; return nullptr; }
    s = r;
    CoopGame::Logf("Steam: signed in as %s (%llu)", s->Name(s->Me()).c_str(), (unsigned long long)s->Me());
    return s;
}

} // namespace CoopSteam
