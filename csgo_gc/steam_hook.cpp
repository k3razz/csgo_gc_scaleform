// this file sucks, don't scroll down!!! all you need to know is
// that this is the bridge betweem the game and ClientGC/ServerGC
#include "stdafx.h"
#include "steam_hook.h"
#include "gc_client.h"
#include "gc_server.h"
#include "platform.h"
#include <funchook.h>

#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>
#include <steam/isteamgamecoordinator.h>

// these are in file scope for networking, callbacks and gc server
// client connect/disconnect notifications
static ClientGC *s_clientGC;
static ServerGC *s_serverGC;

template<size_t N>
inline bool InterfaceMatches(const char *name, const char (&compare)[N])
{
    size_t length = strlen(name);
    if (length != (N - 1))
    {
        return false;
    }

    // compare the full version
    if (!memcmp(name, compare, length))
    {
        return true; // matches
    }

    // compare the base name without the last 3 digits
    if (!memcmp(name, compare, length - 3))
    {
        Platform::Print("Got interface version %s, expecting %s\n", name, compare);
        return false; // not a match
    }

    return false; // not a match
}

// this class sucks but we need to do it this way because
class SteamGameCoordinatorProxy final : public ISteamGameCoordinator
{
    const bool m_server;

public:
    SteamGameCoordinatorProxy(uint64_t steamId)
        : m_server{ !steamId }
    {
        if (m_server)
        {
            assert(!s_serverGC);
            s_serverGC = new ServerGC{};
        }
        else
        {
            assert(!s_clientGC);
            s_clientGC = new ClientGC{ steamId, SteamNetworking() };
        }
    }

    ~SteamGameCoordinatorProxy()
    {
        if (m_server)
        {
            assert(s_serverGC);
            delete s_serverGC;
            s_serverGC = nullptr;
        }
        else
        {
            assert(s_clientGC);
            delete s_clientGC;
            s_clientGC = nullptr;
        }
    }

    EGCResults SendMessage(uint32 unMsgType, const void *pubData, uint32 cubData) override
    {
        if (m_server)
        {
            assert(s_serverGC);
            s_serverGC->HandleMessage(unMsgType, pubData, cubData);
        }
        else
        {
            assert(s_clientGC);
            s_clientGC->HandleMessage(unMsgType, pubData, cubData);
        }

        return k_EGCResultOK;
    }

    bool IsMessageAvailable(uint32 *pcubMsgSize) override
    {
        if (m_server)
        {
            return s_serverGC->HasOutgoingMessages(*pcubMsgSize);
        }
        else
        {
            return s_clientGC->HasOutgoingMessages(*pcubMsgSize);
        }
    }

    EGCResults RetrieveMessage(uint32 *punMsgType, void *pubDest, uint32 cubDest, uint32 *pcubMsgSize) override
    {
        bool result;

        if (m_server)
        {
            result = s_serverGC->PopOutgoingMessage(*punMsgType, pubDest, cubDest, *pcubMsgSize);
        }
        else
        {
            result = s_clientGC->PopOutgoingMessage(*punMsgType, pubDest, cubDest, *pcubMsgSize);
        }

        if (!result)
        {
            if (cubDest < *pcubMsgSize)
            {
                return k_EGCResultBufferTooSmall;
            }

            return k_EGCResultNoMessage;
        }

        return k_EGCResultOK;
    }
};

// stupid hack
constexpr SteamAPICall_t CheckSignatureCall = 0x6666666666666666;

// hook so we can spoof the dll signature checks and get rid of the annoying as fuck insecure message box
class SteamUtilsProxy final : public ISteamUtils
{
private:
    ISteamUtils *const m_original;

public:
    SteamUtilsProxy(ISteamUtils *original)
        : m_original{ original }
    {
    }

    uint32 GetSecondsSinceAppActive() override
    {
        return m_original->GetSecondsSinceAppActive();
    }

    uint32 GetSecondsSinceComputerActive() override
    {
        return m_original->GetSecondsSinceComputerActive();
    }

    EUniverse GetConnectedUniverse() override
    {
        return m_original->GetConnectedUniverse();
    }

    uint32 GetServerRealTime() override
    {
        return m_original->GetServerRealTime();
    }

    const char *GetIPCountry() override
    {
        return m_original->GetIPCountry();
    }

    bool GetImageSize(int iImage, uint32 *pnWidth, uint32 *pnHeight) override
    {
        return m_original->GetImageSize(iImage, pnWidth, pnHeight);
    }

    bool GetImageRGBA(int iImage, uint8 *pubDest, int nDestBufferSize) override
    {
        return m_original->GetImageRGBA(iImage, pubDest, nDestBufferSize);
    }

    bool GetCSERIPPort(uint32 *unIP, uint16 *usPort) override
    {
        return m_original->GetCSERIPPort(unIP, usPort);
    }

    uint8 GetCurrentBatteryPower() override
    {
        return m_original->GetCurrentBatteryPower();
    }

    uint32 GetAppID() override
    {
        return m_original->GetAppID();
    }

    void SetOverlayNotificationPosition(ENotificationPosition eNotificationPosition) override
    {
        m_original->SetOverlayNotificationPosition(eNotificationPosition);
    }

    bool IsAPICallCompleted(SteamAPICall_t hSteamAPICall, bool *pbFailed) override
    {
        if (hSteamAPICall == CheckSignatureCall)
        {
            if (pbFailed)
            {
                *pbFailed = false;
            }

            return true;
        }

        return m_original->IsAPICallCompleted(hSteamAPICall, pbFailed);
    }

    ESteamAPICallFailure GetAPICallFailureReason(SteamAPICall_t hSteamAPICall) override
    {
        // yeah we won't get here
        //if (hSteamAPICall == CheckSignatureCall)
        //{
        //    // not properly handled, shouldn't get here
        //    assert(false);
        //    return k_ESteamAPICallFailureNone;
        //}

        return m_original->GetAPICallFailureReason(hSteamAPICall);
    }

    bool GetAPICallResult(SteamAPICall_t hSteamAPICall, void *pCallback, int cubCallback, int iCallbackExpected, bool *pbFailed) override
    {
        if (hSteamAPICall == CheckSignatureCall
            && cubCallback == sizeof(CheckFileSignature_t)
            && iCallbackExpected == CheckFileSignature_t::k_iCallback)
        {
            if (pbFailed)
            {
                *pbFailed = false;
            }

            CheckFileSignature_t result{};
            result.m_eCheckFileSignature = k_ECheckFileSignatureNoSignaturesFoundForThisApp;
            memcpy(pCallback, &result, sizeof(result));
            return true;
        }

        return m_original->GetAPICallResult(hSteamAPICall, pCallback, cubCallback, iCallbackExpected, pbFailed);
    }

    uint32 GetIPCCallCount() override
    {
        return m_original->GetIPCCallCount();
    }

    void SetWarningMessageHook(SteamAPIWarningMessageHook_t pFunction) override
    {
        m_original->SetWarningMessageHook(pFunction);
    }

protected:
    void RunFrame() override {} // STEAM_PRIVATE_API - protected in 2017

public:
    bool IsOverlayEnabled() override
    {
        return m_original->IsOverlayEnabled();
    }

    bool BOverlayNeedsPresent() override
    {
        return m_original->BOverlayNeedsPresent();
    }

    SteamAPICall_t CheckFileSignature([[maybe_unused]] const char *szFileName) override
    {
        // spoof this
        return CheckSignatureCall;
    }

    bool ShowGamepadTextInput(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, const char *pchDescription, uint32 unCharMax, const char *pchExistingText) override
    {
        return m_original->ShowGamepadTextInput(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText);
    }

    uint32 GetEnteredGamepadTextLength() override
    {
        return m_original->GetEnteredGamepadTextLength();
    }

    bool GetEnteredGamepadTextInput(char *pchText, uint32 cchText) override
    {
        return m_original->GetEnteredGamepadTextInput(pchText, cchText);
    }

    const char *GetSteamUILanguage() override
    {
        return m_original->GetSteamUILanguage();
    }

    bool IsSteamRunningInVR() override
    {
        return m_original->IsSteamRunningInVR();
    }

    void SetOverlayNotificationInset(int nHorizontalInset, int nVerticalInset) override
    {
        m_original->SetOverlayNotificationInset(nHorizontalInset, nVerticalInset);
    }

    bool IsSteamInBigPictureMode() override
    {
        return m_original->IsSteamInBigPictureMode();
    }

    void StartVRDashboard() override
    {
        m_original->StartVRDashboard();
    }
};

class SteamGameServerProxy final : public ISteamGameServer
{
private:
    ISteamGameServer *m_original;

public:
    SteamGameServerProxy(ISteamGameServer *original)
        : m_original{ original }
    {
    }

    bool InitGameServer(uint32 unIP, uint16 usGamePort, uint16 usQueryPort, uint32 unFlags, AppId_t nGameAppId, const char *pchVersionString) override
    {
        // no longer present in steamworks sdk
        constexpr uint32 k_unServerFlagSecureLocal = 2;

        // never run secure!!!
        unFlags &= ~k_unServerFlagSecureLocal;

        // make sure we're up to date
        pchVersionString = "1.99.9.9";

        if ( m_original->InitGameServer(unIP, usGamePort, usQueryPort, unFlags, nGameAppId, pchVersionString))
        {
            // add the csgo_gc gametag
            m_original->SetGameTags("csgo_gc");
            return true;
        }

        return false;
    }

    void SetProduct(const char *pszProduct) override
    {
        m_original->SetProduct(pszProduct);
    }

    void SetGameDescription(const char *pszGameDescription) override
    {
        m_original->SetGameDescription(pszGameDescription);
    }

    void SetModDir(const char *pszModDir) override
    {
        m_original->SetModDir(pszModDir);
    }

    void SetDedicatedServer(bool bDedicated) override
    {
        m_original->SetDedicatedServer(bDedicated);
    }

    void LogOn(const char *pszToken) override
    {
        m_original->LogOn(pszToken);
    }

    void LogOnAnonymous() override
    {
        m_original->LogOnAnonymous();
    }

    void LogOff() override
    {
        m_original->LogOff();
    }

    bool BLoggedOn() override
    {
        return m_original->BLoggedOn();
    }

    bool BSecure() override
    {
        return m_original->BSecure();
    }

    CSteamID GetSteamID() override
    {
        return m_original->GetSteamID();
    }

    bool WasRestartRequested() override
    {
        return m_original->WasRestartRequested();
    }

    void SetMaxPlayerCount(int cPlayersMax) override
    {
        m_original->SetMaxPlayerCount(cPlayersMax);
    }

    void SetBotPlayerCount(int cBotplayers) override
    {
        m_original->SetBotPlayerCount(cBotplayers);
    }

    void SetServerName(const char *pszServerName) override
    {
        m_original->SetServerName(pszServerName);
    }

    void SetMapName(const char *pszMapName) override
    {
        m_original->SetMapName(pszMapName);
    }

    void SetPasswordProtected(bool bPasswordProtected) override
    {
        m_original->SetPasswordProtected(bPasswordProtected);
    }

    void SetSpectatorPort(uint16 unSpectatorPort) override
    {
        m_original->SetSpectatorPort(unSpectatorPort);
    }

    void SetSpectatorServerName(const char *pszSpectatorServerName) override
    {
        m_original->SetSpectatorServerName(pszSpectatorServerName);
    }

    void ClearAllKeyValues() override
    {
        m_original->ClearAllKeyValues();
    }

    void SetKeyValue(const char *pKey, const char *pValue) override
    {
        m_original->SetKeyValue(pKey, pValue);
    }

    void SetGameTags(const char *pchGameTags) override
    {
        std::string tags = pchGameTags;

        if (tags.size())
        {
            tags.append(",csgo_gc");
        }
        else
        {
            tags.append("csgo_gc");
        }

        m_original->SetGameTags(tags.c_str());
    }

    void SetGameData(const char *pchGameData) override
    {
        m_original->SetGameData(pchGameData);
    }

    void SetRegion(const char *pszRegion) override
    {
        m_original->SetRegion(pszRegion);
    }

    HAuthTicket GetAuthSessionTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket) override
    {
        return m_original->GetAuthSessionTicket(pTicket, cbMaxTicket, pcbTicket);
    }

    EBeginAuthSessionResult BeginAuthSession(const void *pAuthTicket, int cbAuthTicket, CSteamID steamID) override
    {
        EBeginAuthSessionResult result = m_original->BeginAuthSession(pAuthTicket, cbAuthTicket, steamID);
        if (s_serverGC && result == k_EBeginAuthSessionResultOK)
        {
            uint64_t clientSteamId = steamID.ConvertToUint64();
            s_serverGC->ClientConnected(clientSteamId, pAuthTicket, cbAuthTicket);

            // For listen-server (offline/bots) mode there is no game-server Steam networking,
            // so the normal P2P handshake (k_EMsgNetworkConnect) never happens and the client
            // never sends its SO cache to the server.  Detect this and wire up a direct in-
            // process channel so that all subsequent GC-to-server messages (equip updates,
            // SO cache) are delivered without going through Steam P2P.
            if (s_clientGC && !SteamGameServerNetworking())
            {
                Platform::Print("Listen-server detected: setting up direct GC channel for %llu\n", clientSteamId);
                s_clientGC->SetListenServer(s_serverGC, clientSteamId);
                s_clientGC->SendSOCacheToGameSever();
            }
        }

        return result;
    }

    void EndAuthSession(CSteamID steamID) override
    {
        if (s_serverGC)
        {
            s_serverGC->ClientDisconnected(steamID.ConvertToUint64());
        }

        m_original->EndAuthSession(steamID);
    }

    void CancelAuthTicket(HAuthTicket hAuthTicket) override
    {
        m_original->CancelAuthTicket(hAuthTicket);
    }

    EUserHasLicenseForAppResult UserHasLicenseForApp(CSteamID steamID, AppId_t appID) override
    {
        return m_original->UserHasLicenseForApp(steamID, appID);
    }

    bool RequestUserGroupStatus(CSteamID steamIDUser, CSteamID steamIDGroup) override
    {
        return m_original->RequestUserGroupStatus(steamIDUser, steamIDGroup);
    }

    void GetGameplayStats() override
    {
        m_original->GetGameplayStats();
    }

    SteamAPICall_t GetServerReputation() override
    {
        return m_original->GetServerReputation();
    }

    uint32 GetPublicIP() override
    {
        return m_original->GetPublicIP();
    }

    bool HandleIncomingPacket(const void *pData, int cbData, uint32 srcIP, uint16 srcPort) override
    {
        return m_original->HandleIncomingPacket(pData, cbData, srcIP, srcPort);
    }

    int GetNextOutgoingPacket(void *pOut, int cbMaxOut, uint32 *pNetAdr, uint16 *pPort) override
    {
        return m_original->GetNextOutgoingPacket(pOut, cbMaxOut, pNetAdr, pPort);
    }

    SteamAPICall_t AssociateWithClan(CSteamID steamIDClan) override
    {
        return m_original->AssociateWithClan(steamIDClan);
    }

    SteamAPICall_t ComputeNewPlayerCompatibility(CSteamID steamIDNewPlayer) override
    {
        return m_original->ComputeNewPlayerCompatibility(steamIDNewPlayer);
    }

    bool SendUserConnectAndAuthenticate(uint32 unIPClient, const void *pvAuthBlob, uint32 cubAuthBlobSize, CSteamID *pSteamIDUser) override
    {
        return m_original->SendUserConnectAndAuthenticate(unIPClient, pvAuthBlob, cubAuthBlobSize, pSteamIDUser);
    }

    CSteamID CreateUnauthenticatedUserConnection() override
    {
        return m_original->CreateUnauthenticatedUserConnection();
    }

    void SendUserDisconnect(CSteamID steamIDUser) override
    {
        m_original->SendUserDisconnect(steamIDUser);
    }

    bool BUpdateUserData(CSteamID steamIDUser, const char *pchPlayerName, uint32 uScore) override
    {
        return m_original->BUpdateUserData(steamIDUser, pchPlayerName, uScore);
    }

    void SetHeartbeatInterval(int iHeartbeatInterval) override
    {
        m_original->SetHeartbeatInterval(iHeartbeatInterval);
    }

    void EnableHeartbeats(bool bActive) override
    {
        m_original->EnableHeartbeats(bActive);
    }

    void ForceHeartbeat() override
    {
        m_original->ForceHeartbeat();
    }
};

class SteamAppsProxy : public ISteamApps
{
    ISteamApps *m_original;

public:
    SteamAppsProxy(ISteamApps *original)
        : m_original{ original }
    {
    }

    bool BIsSubscribed() override
    {
        return true;
    }

    bool BIsSubscribedApp(AppId_t appID) override
    {
        if (appID == 730) return true;
        return m_original->BIsSubscribedApp(appID);
    }

    bool BIsAppInstalled(AppId_t appID) override
    {
        if (appID == 730) return true;
        return m_original->BIsAppInstalled(appID);
    }

    bool GetAppOwnershipTicketExtendedData(uint32 nAppID, void *pvBuffer, uint32 cbBufferLength, uint32 *piAppId, uint32 *piSteamId) override
    {
        if (nAppID == 730)
        {
            if (piAppId) *piAppId = 730;
            if (piSteamId) *piSteamId = 0;
            return true;
        }
        return m_original->GetAppOwnershipTicketExtendedData(nAppID, pvBuffer, cbBufferLength, piAppId, piSteamId);
    }

    uint32 GetAppOwnershipTicket(AppId_t nAppID, void *pvBuffer, uint32 cbBufferLength, uint32 *piAppId) override
    {
        if (nAppID == 730)
        {
            if (piAppId) *piAppId = 730;
            return 0;
        }
        return m_original->GetAppOwnershipTicket(nAppID, pvBuffer, cbBufferLength, piAppId);
    }

    uint32 GetAppData(AppId_t nAppID, const char *pchKey, char *pchValue, int cchValueMax) override
    {
        return m_original->GetAppData(nAppID, pchKey, pchValue, cchValueMax);
    }

    bool BGetDLCDataByIndex(int iDLC, AppId_t *pAppID, bool *pbAvailable, char *pchName, int cchNameBufferSize) override
    {
        return m_original->BGetDLCDataByIndex(iDLC, pAppID, pbAvailable, pchName, cchNameBufferSize);
    }

    int GetDLCCount() override
    {
        return m_original->GetDLCCount();
    }

    bool BIsDlcInstalled(AppId_t nAppID) override
    {
        return m_original->BIsDlcInstalled(nAppID);
    }

    int GetEarliestPurchaseUnixTime(AppId_t nAppID) override
    {
        return m_original->GetEarliestPurchaseUnixTime(nAppID);
    }

    bool BIsVACBanned() override
    {
        return m_original->BIsVACBanned();
    }

    int GetCurrentGameLanguage(char *pchLanguage, int cchLanguageBufferSize) override
    {
        return m_original->GetCurrentGameLanguage(pchLanguage, cchLanguageBufferSize);
    }

    const char *GetCurrentGameLanguage() override
    {
        return m_original->GetCurrentGameLanguage();
    }

    bool BIsCybercafe() override
    {
        return m_original->BIsCybercafe();
    }

    bool BIsLowViolence() override
    {
        return m_original->BIsLowViolence();
    }

    const char *GetAvailableGameLanguages() override
    {
        return m_original->GetAvailableGameLanguages();
    }

    const char *GetLaunchQueryParam(const char *pchKey) override
    {
        return m_original->GetLaunchQueryParam(pchKey);
    }

    bool BGetAchievementProgressStatus(const char *pchAchievementName, int64 *pnUnlocked) override
    {
        return m_original->BGetAchievementProgressStatus(pchAchievementName, pnUnlocked);
    }

    SteamAPICall_t GetFileDetails(const char *pszFileName) override
    {
        return m_original->GetFileDetails(pszFileName);
    }

    int GetNumDlcInstalled() override
    {
        return m_original->GetNumDlcInstalled();
    }

    const char *GetInstalledDLCName(int iIndex) override
    {
        return m_original->GetInstalledDLCName(iIndex);
    }

    AppId_t GetInstalledDLC(int iIndex) override
    {
        return m_original->GetInstalledDLC(iIndex);
    }

    int GetDLCDownloadProgress(AppId_t nAppID, uint64 *puBytesDownloaded, uint64 *puBytesTotal) override
    {
        return m_original->GetDLCDownloadProgress(nAppID, puBytesDownloaded, puBytesTotal);
    }

    bool GetAppInstallDir(AppId_t nAppID, char *pchDir, int cbDir) override
    {
        if (nAppID == 730)
        {
            return m_original->GetAppInstallDir(nAppID, pchDir, cbDir);
        }
        return m_original->GetAppInstallDir(nAppID, pchDir, cbDir);
    }
};

class SteamUserProxy : public ISteamUser
{
    ISteamUser *m_original;

public:
    SteamUserProxy(ISteamUser *original)
        : m_original{ original }
    {
    }

    HSteamUser GetHSteamUser() override
    {
        return m_original->GetHSteamUser();
    }

    bool BLoggedOn() override
    {
        return m_original->BLoggedOn();
    }


    void TerminateGameConnection(uint32 unIPServer, uint16 usPortServer) override
    {
        m_original->TerminateGameConnection(unIPServer, usPortServer);
    }

    void TrackAppUsageEvent(CGameID gameID, int eAppUsageEvent, const char *pchExtraInfo) override
    {
        m_original->TrackAppUsageEvent(gameID, eAppUsageEvent, pchExtraInfo);
    }

    bool GetUserDataFolder(char *pchBuffer, int cubBuffer) override
    {
        return m_original->GetUserDataFolder(pchBuffer, cubBuffer);
    }

    void StartVoiceRecording() override
    {
        m_original->StartVoiceRecording();
    }

    void StopVoiceRecording() override
    {
        m_original->StopVoiceRecording();
    }

    EVoiceResult GetAvailableVoice(uint32 *pcbCompressed, uint32 *pcbUncompressed_Deprecated, uint32 nUncompressedVoiceDesiredSampleRate_Deprecated) override
    {
        return m_original->GetAvailableVoice(pcbCompressed, pcbUncompressed_Deprecated, nUncompressedVoiceDesiredSampleRate_Deprecated);
    }

    EVoiceResult GetVoice(bool bWantCompressed, void *pDestBuffer, uint32 cbDestBufferSize, uint32 *nBytesWritten, bool bWantUncompressed_Deprecated, void *pUncompressedDestBuffer_Deprecated, uint32 cbUncompressedDestBufferSize_Deprecated, uint32 *nUncompressBytesWritten_Deprecated, uint32 nUncompressedVoiceDesiredSampleRate_Deprecated) override
    {
        return m_original->GetVoice(bWantCompressed, pDestBuffer, cbDestBufferSize, nBytesWritten, bWantUncompressed_Deprecated, pUncompressedDestBuffer_Deprecated, cbUncompressedDestBufferSize_Deprecated, nUncompressBytesWritten_Deprecated, nUncompressedVoiceDesiredSampleRate_Deprecated);
    }

    EVoiceResult DecompressVoice(const void *pCompressed, uint32 cbCompressed, void *pDestBuffer, uint32 cbDestBufferSize, uint32 *nBytesWritten, uint32 nDesiredSampleRate) override
    {
        return m_original->DecompressVoice(pCompressed, cbCompressed, pDestBuffer, cbDestBufferSize, nBytesWritten, nDesiredSampleRate);
    }

    uint32 GetVoiceOptimalSampleRate() override
    {
        return m_original->GetVoiceOptimalSampleRate();
    }

    HAuthTicket GetAuthSessionTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket) override
    {
        HAuthTicket ticket = m_original->GetAuthSessionTicket(pTicket, cbMaxTicket, pcbTicket);
        if (s_clientGC && ticket != k_HAuthTicketInvalid)
        {
            s_clientGC->SetAuthTicket(ticket, pTicket, *pcbTicket);
        }

        return ticket;
    }

    EBeginAuthSessionResult BeginAuthSession(const void *pAuthTicket, int cbAuthTicket, CSteamID steamID) override
    {
        EBeginAuthSessionResult result = m_original->BeginAuthSession(pAuthTicket, cbAuthTicket, steamID);
        if (s_serverGC && result == k_EBeginAuthSessionResultOK)
        {
            uint64_t clientSteamId = steamID.ConvertToUint64();
            s_serverGC->ClientConnected(clientSteamId, pAuthTicket, cbAuthTicket);

            if (s_clientGC && !SteamGameServerNetworking())
            {
                Platform::Print("Listen-server detected: setting up direct GC channel for %llu\n", clientSteamId);
                s_clientGC->SetListenServer(s_serverGC, clientSteamId);
                s_clientGC->SendSOCacheToGameSever();
            }
        }
        return result;
    }

    void EndAuthSession(CSteamID steamID) override
    {
        m_original->EndAuthSession(steamID);
    }

    void CancelAuthTicket(HAuthTicket hAuthTicket) override
    {
        if (s_clientGC)
        {
            s_clientGC->ClearAuthTicket(hAuthTicket);
        }

        m_original->CancelAuthTicket(hAuthTicket);
    }

    EUserHasLicenseForAppResult UserHasLicenseForApp(CSteamID steamID, AppId_t appID) override
    {
        if (appID == 730)
        {
            return k_EUserHasLicenseResultHasLicense;
        }
        return m_original->UserHasLicenseForApp(steamID, appID);
    }

    bool BIsBehindNAT() override
    {
        return m_original->BIsBehindNAT();
    }

    void AdvertiseGame(CSteamID steamIDGameServer, uint32 unIPServer, uint16 usPortServer) override
    {
        m_original->AdvertiseGame(steamIDGameServer, unIPServer, usPortServer);
    }

    SteamAPICall_t RequestEncryptedAppTicket(void *pDataToInclude, int cbDataToInclude) override
    {
        return m_original->RequestEncryptedAppTicket(pDataToInclude, cbDataToInclude);
    }

    bool GetEncryptedAppTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket) override
    {
        return m_original->GetEncryptedAppTicket(pTicket, cbMaxTicket, pcbTicket);
    }

    int GetGameBadgeLevel(int nSeries, bool bFoil) override
    {
        return m_original->GetGameBadgeLevel(nSeries, bFoil);
    }

    int GetPlayerSteamLevel() override
    {
        return m_original->GetPlayerSteamLevel();
    }

    SteamAPICall_t RequestStoreAuthURL(const char *pchRedirectURL) override
    {
        return m_original->RequestStoreAuthURL(pchRedirectURL);
    }

    bool BIsPhoneVerified() override
    {
        return m_original->BIsPhoneVerified();
    }

    bool BIsTwoFactorEnabled() override
    {
        return m_original->BIsTwoFactorEnabled();
    }

};

class SteamMatchmakingServersProxy : public ISteamMatchmakingServers
{
    ISteamMatchmakingServers *m_original;

public:
    SteamMatchmakingServersProxy(ISteamMatchmakingServers *original)
        : m_original{ original }
    {
    }

    static MatchMakingKeyValuePair_t *ModifyFilters(MatchMakingKeyValuePair_t *pchFilters, uint32 nFilters, std::vector<MatchMakingKeyValuePair_t> &buffer)
    {
        buffer.reserve(nFilters + 1);
        buffer.assign(pchFilters, pchFilters + nFilters);
        buffer.push_back({ "gametagsand", "csgo_gc" });
        return buffer.data();
    }

    HServerListRequest RequestInternetServerList(AppId_t iApp,
        MatchMakingKeyValuePair_t **ppchFilters,
        uint32 nFilters,
        ISteamMatchmakingServerListResponse *pRequestServersResponse) override
    {
        std::vector<MatchMakingKeyValuePair_t> buffer;
        MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
        return m_original->RequestInternetServerList(iApp, &filters, buffer.size(), pRequestServersResponse);
    }

    HServerListRequest RequestLANServerList(AppId_t iApp,
        ISteamMatchmakingServerListResponse *pRequestServersResponse) override
    {
        return m_original->RequestLANServerList(iApp, pRequestServersResponse);
    }

    HServerListRequest RequestFriendsServerList(AppId_t iApp,
        MatchMakingKeyValuePair_t **ppchFilters,
        uint32 nFilters,
        ISteamMatchmakingServerListResponse *pRequestServersResponse) override
    {
        std::vector<MatchMakingKeyValuePair_t> buffer;
        MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
        return m_original->RequestFriendsServerList(iApp, &filters, buffer.size(), pRequestServersResponse);
    }

    HServerListRequest RequestFavoritesServerList(AppId_t iApp,
        MatchMakingKeyValuePair_t **ppchFilters,
        uint32 nFilters,
        ISteamMatchmakingServerListResponse *pRequestServersResponse) override
    {
        std::vector<MatchMakingKeyValuePair_t> buffer;
        MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
        return m_original->RequestFavoritesServerList(iApp, &filters, buffer.size(), pRequestServersResponse);
    }

    HServerListRequest RequestHistoryServerList(AppId_t iApp,
        MatchMakingKeyValuePair_t **ppchFilters,
        uint32 nFilters,
        ISteamMatchmakingServerListResponse *pRequestServersResponse) override
    {
        std::vector<MatchMakingKeyValuePair_t> buffer;
        MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
        return m_original->RequestHistoryServerList(iApp, &filters, buffer.size(), pRequestServersResponse);
    }

    HServerListRequest RequestSpectatorServerList(AppId_t iApp,
        MatchMakingKeyValuePair_t **ppchFilters,
        uint32 nFilters,
        ISteamMatchmakingServerListResponse *pRequestServersResponse) override
    {
        std::vector<MatchMakingKeyValuePair_t> buffer;
        MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
        return m_original->RequestSpectatorServerList(iApp, &filters, buffer.size(), pRequestServersResponse);
    }

    void ReleaseRequest(HServerListRequest hServerListRequest) override
    {
        m_original->ReleaseRequest(hServerListRequest);
    }

    gameserveritem_t *GetServerDetails(HServerListRequest hRequest, int iServer) override
    {
        return m_original->GetServerDetails(hRequest, iServer);
    }

    void CancelQuery(HServerListRequest hRequest) override
    {
        m_original->CancelQuery(hRequest);
    }

    void RefreshQuery(HServerListRequest hRequest) override
    {
        m_original->RefreshQuery(hRequest);
    }

    bool IsRefreshing(HServerListRequest hRequest) override
    {
        return m_original->IsRefreshing(hRequest);
    }

    int GetServerCount(HServerListRequest hRequest) override
    {
        return m_original->GetServerCount(hRequest);
    }

    void RefreshServer(HServerListRequest hRequest, int iServer) override
    {
        m_original->RefreshServer(hRequest, iServer);
    }

    HServerQuery PingServer(uint32 unIP, uint16 usPort, ISteamMatchmakingPingResponse *pRequestServersResponse) override
    {
        return m_original->PingServer(unIP, usPort, pRequestServersResponse);
    }

    HServerQuery PlayerDetails(uint32 unIP, uint16 usPort, ISteamMatchmakingPlayersResponse *pRequestServersResponse) override
    {
        return m_original->PlayerDetails(unIP, usPort, pRequestServersResponse);
    }

    HServerQuery ServerRules(uint32 unIP, uint16 usPort, ISteamMatchmakingRulesResponse *pRequestServersResponse) override
    {
        return m_original->ServerRules(unIP, usPort, pRequestServersResponse);
    }

    void CancelServerQuery(HServerQuery hServerQuery) override
    {
        m_original->CancelServerQuery(hServerQuery);
    }
};

template<typename Interface, typename Proxy, typename... Args>
inline Interface *GetOrCreate(std::unique_ptr<Proxy> &pointer, Args &&...args)
{
    if (!pointer)
    {
        pointer = std::make_unique<Proxy>(std::forward<Args>(args)...);
    }

    return static_cast<Interface *>(pointer.get());
}

class SteamInterfaceProxy
{
public:
    SteamInterfaceProxy(HSteamPipe pipe)
        : m_pipe{ pipe }
    {
    }

     void *GetInterface(const char *version, void *original)
    {
        if (InterfaceMatches(version, STEAMGAMECOORDINATOR_INTERFACE_VERSION))
        {
            uint64_t steamId = 0;
            if (SteamGameServer_GetHSteamPipe() != m_pipe)
            {
                steamId = SteamUser()->GetSteamID().ConvertToUint64();
            }
            return GetOrCreate<ISteamGameCoordinator>(m_steamGameCoordinator, steamId);
        }
        else if (InterfaceMatches(version, STEAMUTILS_INTERFACE_VERSION))
        {
            return GetOrCreate<ISteamUtils>(m_steamUtils, static_cast<ISteamUtils *>(original));
        }
        else if (InterfaceMatches(version, STEAMGAMESERVER_INTERFACE_VERSION))
        {
            return GetOrCreate<ISteamGameServer>(m_steamGameServer, static_cast<ISteamGameServer *>(original));
        }
        else if (InterfaceMatches(version, STEAMUSER_INTERFACE_VERSION))
        {
            return GetOrCreate<ISteamUser>(m_steamUser, static_cast<ISteamUser *>(original));
        }
        else if (InterfaceMatches(version, STEAMMATCHMAKINGSERVERS_INTERFACE_VERSION))
        {
            return GetOrCreate<ISteamMatchmakingServers>(m_steamMatchmakingServers, static_cast<ISteamMatchmakingServers *>(original));
        }
        else if (InterfaceMatches(version, STEAMAPPS_INTERFACE_VERSION))
        {
            return GetOrCreate<ISteamApps>(m_steamApps, static_cast<ISteamApps *>(original));
        }
    
        return nullptr;
    }
private:
    const HSteamPipe m_pipe;

    std::unique_ptr<SteamGameCoordinatorProxy> m_steamGameCoordinator;
    std::unique_ptr<SteamUtilsProxy> m_steamUtils;
    std::unique_ptr<SteamGameServerProxy> m_steamGameServer;
    std::unique_ptr<SteamUserProxy> m_steamUser;
    std::unique_ptr<SteamMatchmakingServersProxy> m_steamMatchmakingServers;
};

class SteamClientProxy : public ISteamClient
{
    ISteamClient *m_original{};
    ISteamApps *GetISteamApps(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamApps, hSteamUser, hSteamPipe, pchVersion);
    }
    std::unordered_map<uint64_t, SteamInterfaceProxy> m_proxies;
    std::unique_ptr<SteamAppsProxy> m_steamApps;

    uint64_t ProxyKey(HSteamPipe pipe, HSteamUser user)
    {
        return static_cast<uint64_t>(pipe) | (static_cast<uint64_t>(user) << 32);
    }

    SteamInterfaceProxy &GetProxy(HSteamPipe pipe, HSteamUser user, [[maybe_unused]] bool allowNoUser)
    {
        assert(pipe);
        assert(user || allowNoUser);

        auto result = m_proxies.try_emplace(ProxyKey(pipe, user), pipe);
        return result.first->second;
    }

    void RemoveProxy(HSteamPipe pipe, HSteamUser user)
    {
        auto it = m_proxies.find(ProxyKey(pipe, user));
        if (it != m_proxies.end())
        {
            m_proxies.erase(it);
        }
        else
        {
            assert(false);
        }
    }

public:
    void SetOriginal(ISteamClient *original)
    {
        assert(!m_original || m_original == original);
        m_original = original;
    }

    ~SteamClientProxy()
    {
        // debug schizo
        assert(m_proxies.empty());
    }

    HSteamPipe CreateSteamPipe() override
    {
        return m_original->CreateSteamPipe();
    }

    bool BReleaseSteamPipe(HSteamPipe hSteamPipe) override
    {
        // remove proxies not tied to a specific user, e.g. ISteamUtils
        RemoveProxy(hSteamPipe, 0);

        return m_original->BReleaseSteamPipe(hSteamPipe);
    }

    HSteamUser ConnectToGlobalUser(HSteamPipe hSteamPipe) override
    {
        return m_original->ConnectToGlobalUser(hSteamPipe);
    }

    HSteamUser CreateLocalUser(HSteamPipe *phSteamPipe, EAccountType eAccountType) override
    {
        return m_original->CreateLocalUser(phSteamPipe, eAccountType);
    }

    void ReleaseUser(HSteamPipe hSteamPipe, HSteamUser hUser) override
    {
        RemoveProxy(hSteamPipe, hUser);

        m_original->ReleaseUser(hSteamPipe, hUser);
    }

    template<typename T>
    T *ProxyInterface(T *original, HSteamUser user, HSteamPipe pipe, const char *version, bool allowNoUser = false)
    {
        SteamInterfaceProxy &proxy = GetProxy(pipe, user, allowNoUser);
        T *result = static_cast<T *>(proxy.GetInterface(version, original));
        return result ? result : original;
    }

    // temp macro
#define PROXY_INTERFACE(func, user, pipe, version, ...) ProxyInterface(m_original->func(user, pipe, version), user, pipe, version, ##__VA_ARGS__)

    ISteamUser *GetISteamUser(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamUser, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamGameServer *GetISteamGameServer(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamGameServer, hSteamUser, hSteamPipe, pchVersion);
    }

    void SetLocalIPBinding(uint32 unIP, uint16 usPort) override
    {
        m_original->SetLocalIPBinding(unIP, usPort);
    }

    ISteamFriends *GetISteamFriends(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamFriends, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamUtils *GetISteamUtils(HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return ProxyInterface(m_original->GetISteamUtils(hSteamPipe, pchVersion), 0, hSteamPipe, pchVersion, true);
    }

    ISteamMatchmaking *GetISteamMatchmaking(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamMatchmaking, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamMatchmakingServers *GetISteamMatchmakingServers(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamMatchmakingServers, hSteamUser, hSteamPipe, pchVersion);
    }

    void *GetISteamGenericInterface(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamGenericInterface, hSteamUser, hSteamPipe, pchVersion, true);
    }

    ISteamUserStats *GetISteamUserStats(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamUserStats, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamGameServerStats *GetISteamGameServerStats(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamGameServerStats, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamApps *GetISteamApps(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamApps, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamNetworking *GetISteamNetworking(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamNetworking, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamRemoteStorage *GetISteamRemoteStorage(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamRemoteStorage, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamScreenshots *GetISteamScreenshots(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamScreenshots, hSteamuser, hSteamPipe, pchVersion);
    }

    uint32 GetIPCCallCount() override
    {
        return m_original->GetIPCCallCount();
    }

    void SetWarningMessageHook(SteamAPIWarningMessageHook_t pFunction) override
    {
        m_original->SetWarningMessageHook(pFunction);
    }

    bool BShutdownIfAllPipesClosed() override
    {
        return m_original->BShutdownIfAllPipesClosed();
    }

    ISteamHTTP *GetISteamHTTP(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamHTTP, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamUnifiedMessages *GetISteamUnifiedMessages(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamUnifiedMessages, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamController *GetISteamController(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamController, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamUGC *GetISteamUGC(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamUGC, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamAppList *GetISteamAppList(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamAppList, hSteamUser, hSteamPipe, pchVersion);
    }

    ISteamMusic *GetISteamMusic(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamMusic, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamMusicRemote *GetISteamMusicRemote(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamMusicRemote, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamHTMLSurface *GetISteamHTMLSurface(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamHTMLSurface, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamInventory *GetISteamInventory(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamInventory, hSteamuser, hSteamPipe, pchVersion);
    }

    ISteamVideo *GetISteamVideo(HSteamUser hSteamuser, HSteamPipe hSteamPipe, const char *pchVersion) override
    {
        return PROXY_INTERFACE(GetISteamVideo, hSteamuser, hSteamPipe, pchVersion);
    }

protected:
    // These are protected helper methods in the interface that we must implement
    void RunFrame() override {} // STEAM_PRIVATE_API - protected in 2017

    void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void (*func)()) override
    {
        // Can't call protected method on m_original, just ignore
        (void)func;
    }

    void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void (*func)()) override
    {
        // Can't call protected method on m_original, just ignore
        (void)func;
    }

    void Set_SteamAPI_CCheckCallbackRegisteredInProcess(SteamAPI_CheckCallbackRegistered_t func) override
    {
        // Can't call protected method on m_original, just ignore
        (void)func;
    }
};

static SteamClientProxy s_steamClientProxy;

static void *(*Og_CreateInterface)(const char *, int *errorCode);

static void *Hk_CreateInterface(const char *name, int *errorCode)
{
    void *result = Og_CreateInterface(name, errorCode);

    if (InterfaceMatches(name, STEAMCLIENT_INTERFACE_VERSION))
    {
        s_steamClientProxy.SetOriginal(static_cast<ISteamClient *>(result));
        return &s_steamClientProxy;
    }

    return result;
}

struct CallbackHook
{
    int id;
    CCallbackBase *callback;
};

static bool ShouldHookCallback(int id)
{
    // we want to spoof all gc callbacks
    switch (id)
    {
    case GCMessageAvailable_t::k_iCallback:
    case GCMessageFailed_t::k_iCallback:
    case MicroTxnAuthorizationResponse_t::k_iCallback:
        return true;

    default:
        return false;
    }
}

class CallbackAccessor : public CCallbackBase
{
public:
    bool IsGameServer()
    {
        return m_nCallbackFlags & CCallbackBase::k_ECallbackFlagsGameServer;
    }

    void SetRegistered()
    {
        m_nCallbackFlags |= CCallbackBase::k_ECallbackFlagsRegistered;
    }

    void UnsetRegistered()
    {
        m_nCallbackFlags &= ~CCallbackBase::k_ECallbackFlagsRegistered;
    }
};

class CallbackHooks
{
public:
    // returns true if callback was spoofed
    bool RegisterCallback(CCallbackBase *callback, int id)
    {
        if (!ShouldHookCallback(id))
        {
            return false;
        }

        CallbackHook callbackHook{ id, callback };
        m_hooks.push_back(callbackHook);

        static_cast<CallbackAccessor *>(callback)->SetRegistered();
        return true;
    }

    // returns true if callback was spoofed
    bool UnregisterCallback(CCallbackBase *callback)
    {
        bool unregistered = false;

        // iterate over all hooks, just in case...
        for (auto it = m_hooks.begin(); it != m_hooks.end();)
        {
            if (it->callback == callback)
            {
                unregistered = true;
                it = m_hooks.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (unregistered)
        {
            static_cast<CallbackAccessor *>(callback)->UnsetRegistered();
            return true;
        }

        return false;
    }

    // runs callbacks matching id immediately
    void RunCallback(bool server, int id, void *param)
    {
        for (const CallbackHook &hook : m_hooks)
        {
            bool serverCallback = static_cast<CallbackAccessor *>(hook.callback)->IsGameServer();
            if (server == serverCallback && hook.id == id)
            {
                hook.callback->Run(param);
            }
        }
    }

private:
    std::vector<CallbackHook> m_hooks;
};

static CallbackHooks s_callbackHooks;

static void (*Og_SteamAPI_RegisterCallback)(class CCallbackBase *pCallback, int iCallback);
static void (*Og_SteamAPI_UnregisterCallback)(class CCallbackBase *pCallback);
static void (*Og_SteamAPI_RunCallbacks)();
static void (*Og_SteamGameServer_RunCallbacks)();

static void Hk_SteamAPI_RegisterCallback(class CCallbackBase *pCallback, int iCallback)
{
    if (s_callbackHooks.RegisterCallback(pCallback, iCallback))
    {
        return;
    }

    Og_SteamAPI_RegisterCallback(pCallback, iCallback);
}

static void Hk_SteamAPI_UnregisterCallback(class CCallbackBase *pCallback)
{
    if (s_callbackHooks.UnregisterCallback(pCallback))
    {
        return;
    }

    Og_SteamAPI_UnregisterCallback(pCallback);
}

static void Hk_SteamAPI_RunCallbacks()
{
    Og_SteamAPI_RunCallbacks();

    if (s_clientGC)
    {
        uint32_t messageSize;
        if (s_clientGC->HasOutgoingMessages(messageSize))
        {
            GCMessageAvailable_t param{};
            param.m_nMessageSize = messageSize;
            s_callbackHooks.RunCallback(false, GCMessageAvailable_t::k_iCallback, &param);
        }

        MicroTxnAuthorizationResponse_t response;
        response.m_unAppID = 730;
        response.m_ulOrderID = 0;
        response.m_bAuthorized = 1;
        s_callbackHooks.RunCallback(false, MicroTxnAuthorizationResponse_t::k_iCallback, &response);

        s_clientGC->Update();
    }
}

static void Hk_SteamGameServer_RunCallbacks()
{
    Og_SteamGameServer_RunCallbacks();

    if (s_serverGC)
    {
        // run server gc callbacks
        uint32_t messageSize;
        if (s_serverGC->HasOutgoingMessages(messageSize))
        {
            GCMessageAvailable_t param{};
            param.m_nMessageSize = messageSize;
            s_callbackHooks.RunCallback(true, GCMessageAvailable_t::k_iCallback, &param);
        }

        // do networking stuff
        s_serverGC->Update();
    }
}

// shows a message box and exits on failure
static void HookCreate(const char *name, void *target, void *hook, void **bridge)
{
    funchook_t *funchook = funchook_create();
    if (!funchook)
    {
        // unlikely (only allocates) but check anyway
        Platform::Error("funchook_create failed for %s", name);
    }

    void *temp = target;
    int result = funchook_prepare(funchook, &temp, hook);
    if (result != 0)
    {
        Platform::Error("funchook_prepare failed for %s: %s", name, funchook_error_message(funchook));
    }

    *bridge = temp;

    result = funchook_install(funchook, 0);
    if (result != 0)
    {
        Platform::Error("funchook_install failed for %s: %s", name, funchook_error_message(funchook));
    }
}

#define INLINE_HOOK(a) HookCreate(#a, reinterpret_cast<void *>(a), reinterpret_cast<void *>(Hk_##a), reinterpret_cast<void **>(&Og_##a));

static bool InitializeSteamAPI(bool dedicated)
{
    if (dedicated)
    {
        return SteamGameServer_Init(0, 0, 0, MASTERSERVERUPDATERPORT_USEGAMESOCKETSHARE, eServerModeNoAuthentication, "1.38.7.9");
    }
    else
    {
        return SteamAPI_Init();
    }
}

static void ShutdownSteamAPI(bool dedicated)
{
    if (dedicated)
    {
        SteamGameServer_Shutdown();
    }
    else
    {
        SteamAPI_Shutdown();
    }
}

void SteamHookInstall(bool dedicated)
{
    Platform::EnsureEnvVarSet("SteamAppId", "480");

    // this is bit of a clusterfuck
    if (!InitializeSteamAPI(dedicated))
    {
        Platform::Error("Steam initialization failed. Please try the following steps:\n"
                        "- Ensure that Steam is running.\n"
                        "- Restart Steam and try again.\n"
                        "- Verify that you have launched CS:GO or CS2 through Steam at least once.");
    }

    uint8_t steamClientPath[4096]; // NOTE: text encoding stored depends on the platform (wchar_t on windows)
    if (!Platform::SteamClientPath(steamClientPath, sizeof(steamClientPath)))
    {
        Platform::Error("Could not get steamclient module path");
    }

    // decrement reference count
    ShutdownSteamAPI(dedicated);

    // load steamclient
    void *CreateInterface = Platform::SteamClientFactory(steamClientPath);
    if (!CreateInterface)
    {
        Platform::Error("Could not get steamclient factory");
    }

    INLINE_HOOK(CreateInterface);

    // steam api hooks for gc callbacks
    INLINE_HOOK(SteamAPI_RegisterCallback);
    INLINE_HOOK(SteamAPI_UnregisterCallback);
    INLINE_HOOK(SteamAPI_RunCallbacks);
    INLINE_HOOK(SteamGameServer_RunCallbacks);
}
