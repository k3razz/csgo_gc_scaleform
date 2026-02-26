#pragma once

#include "networking_shared.h"

class ClientGC;
class GCMessageRead;
class GCMessageWrite;
class ServerGC;

struct AuthTicket
{
    uint64_t steamId{}; // gameserver
    std::vector<uint8_t> buffer;
};

class NetworkingClient
{
public:
    NetworkingClient(ClientGC *clientGC, ISteamNetworking *networking);

    void Update();

    void SendMessage(const GCMessageWrite &message);

    // Listen-server (offline/LAN) mode: bypass P2P, inject directly into ServerGC
    void SetListenServer(ServerGC *serverGC, uint64_t serverSteamId);

    // for gameserver validation
    void SetAuthTicket(uint32_t handle, const void *data, uint32_t size);
    void ClearAuthTicket(uint32_t handle);

private:
    // return false if it wasn't handled, in which case we pass it to m_clientGC
    bool HandleMessage(uint64_t steamId, GCMessageRead &message);

    ClientGC *const m_clientGC;
    ISteamNetworking *const m_networking;
    uint64_t m_serverSteamId{};

    // listen-server (offline) mode – bypasses P2P entirely
    ServerGC *m_listenServerGC{};
    uint64_t m_listenServerSteamId{};

    std::unordered_map<uint32_t, AuthTicket> m_tickets;

    STEAM_CALLBACK(NetworkingClient,
        OnSessionRequest,
        P2PSessionRequest_t,
        m_sessionRequest);

    STEAM_CALLBACK(NetworkingClient,
        OnSessionFailed,
        P2PSessionConnectFail_t,
        m_sessionFailed);
};
