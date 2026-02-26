#pragma once

#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>

constexpr EP2PSend NetMessageSendType = k_EP2PSendReliable;
constexpr int NetMessageChannel = 7;

// NOTE: these are used as gc message types!
// if they overlap with the game's gc messages, we're doomed
enum ENetworkMsg : uint32_t
{
    // sent by the server to client when they connect, data is the auth ticket
    k_EMsgNetworkConnect = (1u << 31) - 1,
};
