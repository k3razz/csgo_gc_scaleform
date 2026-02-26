#include "stdafx.h"
#include "networking_client.h"
#include "gc_client.h"
#include "gc_server.h"

NetworkingClient::NetworkingClient(ClientGC *clientGC, ISteamNetworking *networking)
    : m_clientGC{ clientGC }
    , m_networking{ networking }
    , m_sessionRequest{ this, &NetworkingClient::OnSessionRequest }
    , m_sessionFailed{ this, &NetworkingClient::OnSessionFailed }
{
}

void NetworkingClient::Update()
{
    uint32_t messageSize;
    while (m_networking->IsP2PPacketAvailable(&messageSize, NetMessageChannel))
    {
        std::vector<uint8_t> buffer(messageSize);
        CSteamID steamId;
        uint32_t bytesRead = 0;
        
        if (!m_networking->ReadP2PPacket(buffer.data(), messageSize, &bytesRead, &steamId, NetMessageChannel))
        {
            assert(false);
            continue;
        }

        uint64_t steamId64 = steamId.ConvertToUint64();

        // pass 0 as type so it gets parsed from the message
        GCMessageRead messageRead{ 0, buffer.data(), bytesRead };
        if (!messageRead.IsValid())
        {
            assert(false);
            continue;
        }

        if (HandleMessage(steamId64, messageRead))
        {
            // that was an internal message
            continue;
        }

        // don't pass messages to the gc unless it's our gameserver
        if (!m_serverSteamId || steamId64 != m_serverSteamId)
        {
            Platform::Print("NetworkingClient: ignored message from %llu (not our gs %llu)\n", steamId64, m_serverSteamId);
            continue;
        }

        // let the gc have a whack at it
        m_clientGC->HandleNetMessage(messageRead);
    }
}

static bool ValidateTicket(std::unordered_map<uint32_t, AuthTicket> &tickets, uint64_t steamId, const void *data, uint32_t size)
{
    for (auto &pair : tickets)
    {
        if (pair.second.buffer.size() == size && !memcmp(pair.second.buffer.data(), data, size))
        {
            pair.second.steamId = steamId;
            return true;
        }
    }

    return false;
}

bool NetworkingClient::HandleMessage(uint64_t steamId, GCMessageRead &message)
{
    if (message.IsProtobuf())
    {
        // internal messages are not protobuf based
        return false;
    }

    uint32_t typeUnmasked = message.TypeUnmasked();
    if (typeUnmasked == k_EMsgNetworkConnect)
    {
        uint32_t ticketSize = message.ReadUint32();
        const void *ticket = message.ReadData(ticketSize);
        if (!message.IsValid())
        {
            Platform::Print("NetworkingClient: ignored connection from %llu (malfored message)\n", steamId);
            return true;
        }

        if (!ValidateTicket(m_tickets, steamId, ticket, ticketSize))
        {
            Platform::Print("NetworkingClient: ignored connection from %llu (ticket mismatch)\n", steamId);
            return true;
        }

        Platform::Print("NetworkingClient: sending socache to %llu\n", steamId);
        m_serverSteamId = steamId;
        m_clientGC->SendSOCacheToGameSever();

        return true;
    }

    return false;
}

void NetworkingClient::SetListenServer(ServerGC *serverGC, uint64_t serverSteamId)
{
    m_listenServerGC = serverGC;
    m_listenServerSteamId = serverSteamId;
}

void NetworkingClient::SendMessage(const GCMessageWrite &message)
{
    if (m_serverSteamId)
    {
        // connected via P2P (dedicated server or LAN)
        CSteamID steamId;
        steamId.SetFromUint64(m_serverSteamId);

        bool result = m_networking->SendP2PPacket(
            steamId,
            message.Data(),
            message.Size(),
            NetMessageSendType,
            NetMessageChannel);

        assert(result);
        (void)result;
    }
    else if (m_listenServerGC)
    {
        // listen-server (offline/bots) mode: inject the GC message directly into the server
        // since there is no game-server Steam networking available to do P2P with ourselves
        m_listenServerGC->HandleNetMessage(m_listenServerSteamId, message.Data(), message.Size());
    }
    // else: not connected to any server, silently drop
}

void NetworkingClient::SetAuthTicket(uint32_t handle, const void *data, uint32_t size)
{
    AuthTicket &ticket = m_tickets[handle];
    ticket.steamId = 0;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    ticket.buffer.assign(bytes, bytes + size);
}

void NetworkingClient::ClearAuthTicket(uint32_t handle)
{
    auto it = m_tickets.find(handle);
    if (it == m_tickets.end())
    {
        assert(false);
        Platform::Print("NetworkingClient: tried to clear a nonexistent auth ticket???\n");
        return;
    }

    if (it->second.steamId)
    {
        Platform::Print("NetworkingClient: closing p2p session with %llu\n", it->second.steamId);

        // we had a session so close the connection
        CSteamID steamId;
        steamId.SetFromUint64(it->second.steamId);
        m_networking->CloseP2PChannelWithUser(steamId, NetMessageChannel);

        // was this our current gameserver? if it was, clear it
        if (it->second.steamId == m_serverSteamId)
        {
            Platform::Print("NetworkingClient: clearing gs identity\n");
            m_serverSteamId = 0;
        }
    }

    m_tickets.erase(it);
}

void NetworkingClient::OnSessionRequest(P2PSessionRequest_t *param)
{
    if (!param->m_steamIDRemote.BGameServerAccount())
    {
        // csgo_gc related connections come from gameservers
        return;
    }

    // accept the connection, we should receive the k_EMsgNetworkConnect message
    m_networking->AcceptP2PSessionWithUser(param->m_steamIDRemote);
}

void NetworkingClient::OnSessionFailed(P2PSessionConnectFail_t *param)
{
    Platform::Print("NetworkingClient::OnSessionFailed: P2P session error %d\n", param->m_eP2PSessionError);
}
