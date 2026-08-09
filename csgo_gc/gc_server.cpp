#include "stdafx.h"
#include "gc_server.h"
#include "gc_const.h"
#include "gc_const_csgo.h"
#include "graffiti.h"
#include <random>

const char *MessageName(uint32_t type);

static std::random_device s_rd;
static std::mt19937 s_gen(s_rd());

static const std::vector<uint32_t> s_availableSkins = {
    7, 8, 9, 11, 13, 14, 16, 19, 22, 26, 28, 30, 32,
    34, 35, 36, 38, 39, 40, 41, 42, 43, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83
};

bool IsBot(uint64_t steamId)
{
    return (steamId == 0 || steamId == 0xFFFFFFFFFFFFFFFF || steamId > 0xFFFFFFFF00000000ULL);
}

uint32_t GetRandomSkinDefIndex()
{
    std::uniform_int_distribution<> dis(0, s_availableSkins.size() - 1);
    return s_availableSkins[dis(s_gen)];
}

ServerGC::ServerGC()
{
    Platform::Print("ServerGC spawned\n");
    Graffiti::Initialize();
}

ServerGC::~ServerGC()
{
    Platform::Print("ServerGC destroyed\n");
}

void ServerGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
{
    GCMessageRead messageRead{ type, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCServerHello:
            OnServerHello(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Server2GCClientValidate:
            break;

        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            break;

        default:
            Platform::Print("ServerGC::HandleMessage: unhandled protobuf message %s)\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
    else
    {
        uint32_t typeUnmasked = messageRead.TypeUnmasked();
        Platform::Print("[ServerGC] HandleMessage: struct message type=%u\n", typeUnmasked);
    }
}

void ServerGC::ClientConnected(uint64_t steamId, const void *ticket, uint32_t ticketSize)
{
    Platform::Print("ClientConnected: %llu\n", steamId);
    m_networking.ClientConnected(steamId, ticket, ticketSize);
    
    if (IsBot(steamId))
    {
        uint32_t skin = GetRandomSkinDefIndex();
        Platform::Print("[ServerGC] Bot %llu assigned skin %u\n", steamId, skin);
    }
}

void ServerGC::ClientDisconnected(uint64_t steamId)
{
    Platform::Print("ClientDisconnected: %llu\n", steamId);
    m_networking.ClientDisconnected(steamId);

    CMsgSOCacheUnsubscribed message;
    message.mutable_owner_soid()->set_type(SoIdTypeSteamId);
    message.mutable_owner_soid()->set_id(steamId);

    m_outgoingMessages.emplace(k_ESOMsg_CacheUnsubscribed, message);
}

void ServerGC::Update()
{
    if (!m_receivedHello)
    {
        return;
    }

    uint64_t steamId;
    std::vector<uint8_t> data;
    while (m_networking.ReceiveMessage(steamId, data))
    {
        HandleNetMessage(steamId, data.data(), data.size());
    }
}

template<typename T>
static bool ValidateMessageOwnerSOID(GCMessageRead &messageRead, uint64_t steamId)
{
    T message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("ValidateMessageOwnerSOID %llu: parsing failed\n", steamId);
        return false;
    }

    if (message.owner_soid().type() != SoIdTypeSteamId
        || message.owner_soid().id() != steamId)
    {
        Platform::Print("ValidateMessageOwnerSOID %llu: steam id mismatch (message has %llu)\n",
            steamId, message.owner_soid().id());
        return false;
    }

    return true;
}

void ServerGC::HandleNetMessage(uint64_t steamId, const void *data, uint32_t size)
{
    GCMessageRead validate{ 0, data, size };
    if (!validate.IsValid())
    {
        assert(false);
        return;
    }

    if (validate.TypeUnmasked() == k_ESOMsg_CacheSubscribed)
    {
        Platform::Print("[ServerGC] CacheSubscribed: always granting access\n");
        m_outgoingMessages.emplace(data, size);
        return;
    }

    if (!validate.IsProtobuf())
    {
        Platform::Print("ServerGC: ignoring non protobuf message %u from %llu\n",
            validate.TypeUnmasked(), steamId);
        return;
    }

    uint32_t type = validate.TypeUnmasked();
    
    if (type == k_EMsgGCItemCustomizationNotification ||
        type == k_EMsgGCShowItemsPickedUp ||
        type == k_EMsgGCStorePurchaseInitResponse ||
        type == k_EMsgGCStorePurchaseFinalizeResponse ||
        type == k_EMsgGCUnlockCrateResponse ||
        type == k_EMsgGCNameItemNotification ||
        type == k_EMsgGCUseItemResponse ||
        type == k_EMsgGCCraftResponse)
    {
        Platform::Print("[ServerGC] Blocked echo of notification message %u from %llu\n", type, steamId);
        return;
    }

    bool isValid = false;

    switch (type)
    {
    case k_ESOMsg_Create:
    case k_ESOMsg_Update:
    case k_ESOMsg_Destroy:
        isValid = ValidateMessageOwnerSOID<CMsgSOSingleObject>(validate, steamId);
        break;

    case k_ESOMsg_CacheSubscribed:
        isValid = ValidateMessageOwnerSOID<CMsgSOCacheSubscribed>(validate, steamId);
        break;

    case k_ESOMsg_UpdateMultiple:
        isValid = ValidateMessageOwnerSOID<CMsgSOMultipleObjects>(validate, steamId);
        break;

    case k_EMsgGCItemAcknowledged:
        isValid = true;
        break;
    }

    if (!isValid)
    {
        Platform::Print("ServerGC: ignoring net message %u from %llu\n",
            validate.TypeUnmasked(), steamId);
        return;
    }

    m_outgoingMessages.emplace(data, size);
}

void ServerGC::OnServerHello(GCMessageRead &messageRead)
{
    CMsgServerHello hello;
    if (!messageRead.ReadProtobuf(hello))
    {
        Platform::Print("Parsing CMsgServerHello failed, ignoring\n");
        return;
    }

    Platform::Print("ServerGC received ServerHello\n");

    CMsgCStrike15Welcome csWelcome;
    csWelcome.set_gscookieid(GameServerCookieId);

    CMsgClientWelcome welcome;
    welcome.set_version(0);
    welcome.set_game_data(csWelcome.SerializeAsString());
    welcome.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));

    m_outgoingMessages.emplace(k_EMsgGCServerWelcome, welcome);

    m_receivedHello = true;
    Platform::Print("ServerGC sent ServerWelcome and is ready\n");
}

void ServerGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    GCMessageWrite messageWrite{ k_EMsgGC_IncrementKillCountAttribute, message };
    CSteamID killerId{ message.killer_account_id(), k_EUniversePublic, k_EAccountTypeIndividual };
    m_networking.SendMessage(killerId.ConvertToUint64(), messageWrite);
}
