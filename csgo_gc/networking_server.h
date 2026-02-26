#pragma once

#include "networking_shared.h"

class GCMessageWrite;

class NetworkingServer
{
public:
    NetworkingServer();

    // receive a message, returns true if a message was available
    bool ReceiveMessage(uint64_t &steamId, std::vector<uint8_t> &data);

    void ClientConnected(uint64_t steamId, const void *ticket, uint32_t ticketSize);
    void ClientDisconnected(uint64_t steamId);

    void SendMessage(uint64_t steamId, const GCMessageWrite &message);

private:
    // fetched lazily because SteamGameServerNetworking() is null at construction time
    ISteamNetworking *GetNetworking();
    ISteamNetworking *m_networking{};
    std::unordered_set<uint64_t> m_clients;

    STEAM_GAMESERVER_CALLBACK(NetworkingServer,
        OnSessionRequest,
        P2PSessionRequest_t,
        m_sessionRequest);

    STEAM_GAMESERVER_CALLBACK(NetworkingServer,
        OnSessionFailed,
        P2PSessionConnectFail_t,
        m_sessionFailed);
};
