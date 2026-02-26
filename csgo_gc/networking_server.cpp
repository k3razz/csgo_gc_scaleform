#include "stdafx.h"
#include "networking_server.h"
#include "gc_message.h"

NetworkingServer::NetworkingServer()
    : m_sessionRequest{ this, &NetworkingServer::OnSessionRequest }
    , m_sessionFailed{ this, &NetworkingServer::OnSessionFailed }
{
}

ISteamNetworking *NetworkingServer::GetNetworking()
{
    if (m_networking)
        return m_networking;
    
    // Try gameserver networking first (dedicated server mode)
    m_networking = SteamGameServerNetworking();
    if (m_networking)
    {
        Platform::Print("NetworkingServer: using SteamGameServerNetworking()\n");
        return m_networking;
    }
    
    // Fallback to client networking (listen server mode)
    // In listen server, both client and server use the same Steam context
    m_networking = SteamNetworking();
    if (m_networking)
    {
        Platform::Print("NetworkingServer: falling back to SteamNetworking() for listen server\n");
        return m_networking;
    }
    
    Platform::Print("NetworkingServer: SteamNetworking() is also null!\n");
    return nullptr;
}

bool NetworkingServer::ReceiveMessage(uint64_t &steamId, std::vector<uint8_t> &data)
{
    ISteamNetworking *networking = GetNetworking();
    if (!networking)
    {
        // no gameserver networking in listen server
        return false;
    }

    uint32_t messageSize;
    if (!networking->IsP2PPacketAvailable(&messageSize, NetMessageChannel))
    {
        return false;
    }

    Platform::Print("NetworkingServer: received P2P packet of size %u\n", messageSize);

    data.resize(messageSize);
    CSteamID steamIdObj;
    uint32_t bytesRead = 0;
    if (!networking->ReadP2PPacket(data.data(), messageSize, &bytesRead, &steamIdObj, NetMessageChannel))
    {
        Platform::Print("NetworkingServer: ReadP2PPacket failed\n");
        return false;
    }

    steamId = steamIdObj.ConvertToUint64();

    // see if we have a session
    auto it = m_clients.find(steamId);
    if (it == m_clients.end())
    {
        Platform::Print("NetworkingServer: ignored message from %llu (no session)\n", steamId);
        return false;
    }

    data.resize(bytesRead);
    return true;
}

// helper for ISteamNetworking::SendP2PPacket that attempts to do some kind of error handling
static void SendMessageToUser(ISteamNetworking *networking, uint64_t steamId, const GCMessageWrite &message)
{
    CSteamID steamIdObj;
    steamIdObj.SetFromUint64(steamId);

    bool result = networking->SendP2PPacket(
        steamIdObj,
        message.Data(),
        message.Size(),
        NetMessageSendType,
        NetMessageChannel);

    if (!result)
    {
        Platform::Print("SendP2PPacket failed for %llu, closing session and trying again\n", steamId);

        networking->CloseP2PChannelWithUser(steamIdObj, NetMessageChannel);

        result = networking->SendP2PPacket(
            steamIdObj,
            message.Data(),
            message.Size(),
            NetMessageSendType,
            NetMessageChannel);

        if (!result)
        {
            // not much we can do in this situation i guess
            Platform::Print("SendP2PPacket failed for %llu\n", steamId);
        }
    }
}

void NetworkingServer::ClientConnected(uint64_t steamId, const void *ticket, uint32_t ticketSize)
{
    auto [it, added] = m_clients.insert(steamId);
    if (!added)
    {
        Platform::Print("got ClientConnected for %llu but they're already on the list! ignoring\n", steamId);
        return;
    }

    ISteamNetworking *networking = GetNetworking();
    if (!networking)
    {
        // no gameserver networking in listen server, skip P2P message
        Platform::Print("ClientConnected: no gameserver networking available (listen server), skipping P2P\n");
        return;
    }

    // send a message, if the client has csgo_gc installed they will
    // reply with their so cache and we'll add them to our list
    GCMessageWrite messageWrite{ k_EMsgNetworkConnect };
    messageWrite.WriteUint32(ticketSize);
    messageWrite.WriteData(ticket, ticketSize);

    // FIXME: this gets sent when the client is connecting to the server, it's not uncommon for
    // the connection to time out, in which case the player's socache never gets to the server
    SendMessageToUser(networking, steamId, messageWrite);
}

void NetworkingServer::ClientDisconnected(uint64_t steamId)
{
    auto it = m_clients.find(steamId);
    if (it == m_clients.end())
    {
        Platform::Print("got ClientDisconnected for %llu but they're not on the list! ignoring\n", steamId);
        return;
    }

    m_clients.erase(it);

    ISteamNetworking *networking = GetNetworking();
    if (!networking)
    {
        return;
    }

    CSteamID steamIdObj;
    steamIdObj.SetFromUint64(steamId);
    networking->CloseP2PChannelWithUser(steamIdObj, NetMessageChannel);
}

void NetworkingServer::SendMessage(uint64_t steamId, const GCMessageWrite &message)
{
    auto it = m_clients.find(steamId);
    if (it == m_clients.end())
    {
        Platform::Print("No csgo_gc session with %llu, not sending message!!!\n");
        return;
    }

    ISteamNetworking *networking = GetNetworking();
    if (!networking)
    {
        // no gameserver networking in listen server
        return;
    }

    SendMessageToUser(networking, steamId, message);
}

void NetworkingServer::OnSessionRequest(P2PSessionRequest_t *param)
{
    uint64_t steamId = param->m_steamIDRemote.ConvertToUint64();

    auto it = m_clients.find(steamId);
    if (it == m_clients.end())
    {
        Platform::Print("%llu sent a session request, we don't have a csgo_gc session, ignoring...\n", steamId);
        return;
    }

    ISteamNetworking *networking = GetNetworking();
    if (!networking)
    {
        return;
    }

    Platform::Print("%llu sent a session request, we were playing GC with them so accept\n", steamId);

    if (!networking->AcceptP2PSessionWithUser(param->m_steamIDRemote))
    {
        Platform::Print("AcceptP2PSessionWithUser with %llu failed???\n", steamId);
    }
}

void NetworkingServer::OnSessionFailed(P2PSessionConnectFail_t *param)
{
    // don't do anything, rely on the auth session
    Platform::Print("OnSessionFailed: P2P session error %d\n", param->m_eP2PSessionError);
}
