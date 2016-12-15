// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"

#include <CryLobby/ICrySignIn.h>
#include "GameBrowser.h"
#include "GameLobbyData.h"

#include <CryLobby/ICryLobby.h>
#include <CryLobby/ICryMatchMaking.h>
#include <CryGame/IGameFramework.h>
#include "IPlayerProfiles.h"

#include "DLCManager.h"
#include "PersistantStats.h"
#include "PlaylistManager.h"
#include "Game.h"

#include <CryCore/CryEndian.h>

#include "GameCVars.h"
#include <CryString/StringUtils.h>
#include "GameCodeCoverage/GameCodeCoverageTracker.h"
#include "Network/Squad/SquadManager.h"
#include "Network/Lobby/GameLobbyCVars.h"
#include "RichPresence.h"

#include <IPlayerProfiles.h>

#if defined(USE_SESSION_SEARCH_SIMULATOR)
#include "SessionSearchSimulator.h"
#endif
#include "UI/UILobbyMP.h"
#include <CrySystem/ISystem.h>
#include "UI/UIManager.h"

#if CRY_PLATFORM_DURANGO && !defined(DURANGO_LIVE_TITLE_ID)
// added hardcoded defines inside CryGameSDK Preprocessor definitions to maintain legacy solution
#include <durango_title_id.h> // file is auto generated by the build system (only used by WAF)
#endif

static char s_profileName[CRYLOBBY_USER_NAME_LENGTH] = "";

#define START_SEARCHING_FOR_SERVERS_NUM_DATA	3

#define MAX_NUM_PER_FRIEND_ID_SEARCH 15	// Number of favourite Id's to search for at once.
#define MIN_TIME_BETWEEN_SEARCHES 1.f		// Time in secs before performing another search query. See m_delayedSearchType. Excludes favouriteId searches.

//-------------------------------------------------------------------------
CGameBrowser::CGameBrowser( void )
{
#if defined(USE_SESSION_SEARCH_SIMULATOR)
	m_pSessionSearchSimulator = NULL;
#endif //defined(USE_SESSION_SEARCH_SIMULATOR)

	m_NatType = eNT_Unknown;

	m_searchingTask = CryLobbyInvalidTaskID;

	m_bFavouriteIdSearch = false;

	m_delayedSearchType = eDST_None;

	m_lastSearchTime = 0.f;

#if IMPLEMENT_PC_BLADES
	m_currentFavouriteIdSearchType = CGameServerLists::eGSL_Favourite;
	m_currentSearchFavouriteIdIndex = 0;
	m_numSearchFavouriteIds = 0;
	memset(m_searchFavouriteIds, INVALID_SESSION_FAVOURITE_ID, sizeof(m_searchFavouriteIds));
#endif

	Init(); // init and trigger a request to register data - needs to be done early.
}

//-------------------------------------------------------------------------
void CGameBrowser::Init( void )
{
	gEnv->pNetwork->GetLobby()->RegisterEventInterest(eCLSE_NatType, CGameBrowser::GetNatTypeCallback, this);

#if defined(USE_SESSION_SEARCH_SIMULATOR)
	m_pSessionSearchSimulator = new CSessionSearchSimulator();
#endif //defined(USE_SESSION_SEARCH_SIMULATOR)
}

//--------------------------------------------------------------------------
CGameBrowser::~CGameBrowser()
{
#ifdef CODECHECKPOINT_ENABLED
	gEnv->pCodeCheckpointMgr->UnRegisterCheckpoint("GameLobby_StartSearchingForServers");
#endif // CODECHECKPOINT_ENABLED

#if defined(USE_SESSION_SEARCH_SIMULATOR)
	SAFE_DELETE( m_pSessionSearchSimulator );
#endif //defined(USE_SESSION_SEARCH_SIMULATOR)
}

//-------------------------------------------------------------------------
void CGameBrowser::StartSearchingForServers(CryMatchmakingSessionSearchCallback cb)
{
	ICryLobby *lobby = gEnv->pNetwork->GetLobby();
	if (lobby != NULL && lobby->GetLobbyService())
	{
		CCCPOINT (GameLobby_StartSearchingForServers);

		if (CanStartSearch())
		{
			CryLog("[UI] Delayed Searching for sessions");
			m_delayedSearchType = eDST_Full;

			NOTIFY_UILOBBY_MP(SearchStarted());

			return;
		}

		SCrySessionSearchParam param;
		SCrySessionSearchData data[START_SEARCHING_FOR_SERVERS_NUM_DATA];

		param.m_type = REQUIRED_SESSIONS_QUERY;
		param.m_data = data;
		param.m_numFreeSlots = 0; 
		param.m_maxNumReturn = g_pGameCVars->g_maxGameBrowserResults;
		param.m_ranked = false;

		int curData = 0;
		CRY_ASSERT_MESSAGE( curData < START_SEARCHING_FOR_SERVERS_NUM_DATA, "Session search data buffer overrun" );
		data[curData].m_operator = eCSSO_Equal;
		data[curData].m_data.m_id = LID_MATCHDATA_VERSION;
		data[curData].m_data.m_type = eCLUDT_Int32;
		data[curData].m_data.m_int32 = GameLobbyData::GetVersion();
		++curData;


		// if you want to use this, make sure the search query in the SPA asks for this param as well
		if (!g_pGameCVars->g_ignoreDLCRequirements)
		{
			// Note: GetSquadCommonDLCs is actually a bit field, so it should really be doing a bitwise & to determine
			// if the client can join the server. However this is not supported so the less than equal operator
			// is used instead. This may return some false positives but never any false negatives, the false
			// positives will be filtered out when the results are retreived.
			CRY_ASSERT_MESSAGE( curData < START_SEARCHING_FOR_SERVERS_NUM_DATA, "Session search data buffer overrun" );
			data[curData].m_operator = eCSSO_LessThanEqual;
			data[curData].m_data.m_id = LID_MATCHDATA_REQUIRED_DLCS;
			data[curData].m_data.m_type = eCLUDT_Int32;
			data[curData].m_data.m_int32 = g_pGame->GetDLCManager()->GetSquadCommonDLCs();
			++curData;
		}

		param.m_numData = curData;

		//CRY_ASSERT_MESSAGE(m_searchingTask==CryLobbyInvalidTaskID,"CGameBrowser Trying to search for sessions when you think you are already searching.");

		ECryLobbyError error = StartSearchingForServers(&param, cb, this, false);

		//CRY_ASSERT_MESSAGE(error==eCLE_Success,"CGameBrowser searching for sessions failed.");

		if (error == eCLE_Success)
		{
			NOTIFY_UILOBBY_MP(SearchStarted());

			CryLogAlways("CCGameBrowser::StartSearchingForServers %d", m_searchingTask);
		}
		else
		{
			NOTIFY_UILOBBY_MP(SearchCompleted());

			m_searchingTask = CryLobbyInvalidTaskID;
		}
	}
	else
	{
		CRY_ASSERT_MESSAGE(0,"CGameBrowser Cannot search for servers : no lobby service available.");
	}
}

//-------------------------------------------------------------------------
ECryLobbyError CGameBrowser::StartSearchingForServers(SCrySessionSearchParam* param, CryMatchmakingSessionSearchCallback cb, void* cbArg, const bool bFavouriteIdSearch)
{
	m_bFavouriteIdSearch = bFavouriteIdSearch;
	m_delayedSearchType = eDST_None;
	m_lastSearchTime = gEnv->pTimer->GetCurrTime(); 

#if defined(USE_SESSION_SEARCH_SIMULATOR)	
	if( m_pSessionSearchSimulator && CGameLobbyCVars::Get()->gl_searchSimulatorEnabled )
	{
		const char* filepath = gEnv->pConsole->GetCVar( "gl_searchSimulatorFilepath" )->GetString();
		if( filepath != NULL && strcmpi( filepath, m_pSessionSearchSimulator->GetCurrentFilepath() ) != 0  )
		{
			m_pSessionSearchSimulator->OpenSessionListXML( filepath );
		}

		m_pSessionSearchSimulator->OutputSessionListBlock( m_searchingTask, cb, cbArg );
		return eCLE_Success;
	}
	else
#endif //defined(USE_SESSION_SEARCH_SIMULATOR)
	{
		ECryLobbyError error = eCLE_ServiceNotSupported;
		ICryLobby *lobby = gEnv->pNetwork->GetLobby();
		uint32 userIndex = g_pGame->GetExclusiveControllerDeviceIndex();
		if(lobby)
		{
			CryLobbyTaskID previousTask = m_searchingTask;
			error = lobby->GetLobbyService()->GetMatchMaking()->SessionSearch(userIndex, param, &m_searchingTask, cb, cbArg);
			CryLog("CGameBrowser::StartSearchingForServers previousTask=%u, newTask=%u", previousTask, m_searchingTask);
		}
		return error;
	}
}

//-------------------------------------------------------------------------
void CGameBrowser::CancelSearching(bool feedback /*= true*/)
{
	CryLog("CGameBrowser::CancelSearching");

	if (m_searchingTask != CryLobbyInvalidTaskID)
	{
		CryLog("  canceling search task %u", m_searchingTask);
		ICryLobby *pLobby = gEnv->pNetwork->GetLobby();
		pLobby->GetMatchMaking()->CancelTask(m_searchingTask);
		// Calling FinishedSearch will clear m_searchingTask
	}

	m_delayedSearchType = eDST_None;

	FinishedSearch(feedback, true);
}

//-------------------------------------------------------------------------
void CGameBrowser::FinishedSearch(bool feedback, bool finishedSearch)
{
	CryLog("CGameBrowser::FinishedSearch()");
	m_searchingTask = CryLobbyInvalidTaskID;

#if IMPLEMENT_PC_BLADES
	if (m_bFavouriteIdSearch)
	{
		bool bSearchOver = true;

		if (!finishedSearch && (m_currentSearchFavouriteIdIndex < m_numSearchFavouriteIds))
		{
			if (DoFavouriteIdSearch())
			{
				feedback = false;
				bSearchOver = false;
			}
		}

		if (bSearchOver)	
		{
			for (uint32 i = 0; i < m_numSearchFavouriteIds; ++i)
			{
				if (m_searchFavouriteIds[i] != INVALID_SESSION_FAVOURITE_ID)
				{
					g_pGame->GetGameServerLists()->ServerNotFound(m_currentFavouriteIdSearchType, m_searchFavouriteIds[i]);
				}

				m_searchFavouriteIds[i] = INVALID_SESSION_FAVOURITE_ID;
			}
		}
	}
#endif
		
	if(feedback)
	{
		NOTIFY_UILOBBY_MP(SearchCompleted());
	}
}

bool CGameBrowser::CanStartSearch()
{
	const float currTime = gEnv->pTimer->GetCurrTime();
	return m_searchingTask == CryLobbyInvalidTaskID // No search running
		&& (m_lastSearchTime + MIN_TIME_BETWEEN_SEARCHES) >= currTime; // Timout
}

//---------------------------------------
void CGameBrowser::Update(const float dt)
{
	if (m_delayedSearchType != eDST_None)
	{
		if (CanStartSearch())
		{
			CryLog("[UI] Activate delayed search %d", m_delayedSearchType);
			if (m_delayedSearchType == eDST_Full)
			{
				StartSearchingForServers();
			}
			else if (m_delayedSearchType == eDST_FavouriteId)
			{
				if (!DoFavouriteIdSearch())
				{
					NOTIFY_UILOBBY_MP(SearchCompleted());
				}
			}

			m_delayedSearchType = eDST_None;
		}
	}
}

//------------
// CALLBACKS
//------------

// This is a new callback since adding PSN support
//Basically whenever this callback is fired, you are required to fill in the requestedParams that are asked for.
//At present specifications are that the callback can fire multiple times.
//
// 'PCom'		void*		ptr to static sceNpCommunitcationId					(not copied - DO NOT PLACE ON STACK!)
// 'PPas'		void*		ptr to static sceNpCommunicationPassphrase	(not copied - DO NOT PLACE ON STACK!)
// 'XSvc'		uint32	Service Id for XLSP servers
// 'XPor'		uint16	Port for XLSP server to communicate with Telemetry
// 'LUnm'		void*		ptr to user name for local user - used by LAN (due to lack of guid) (is copied internally - DO NOT PLACE ON STACK)
//

#if USE_STEAM
#define STEAM_APPID (220980)
#endif // USE_STEAM

#if CRY_PLATFORM_ORBIS

// For security reasons these values must NOT be placed outside of the executable (e.g. in xml data for instance!)
#include <np.h>

static const SceNpTitleId s_title_id = {
	{'N','P','X','X','0','0','0','0','0','_','0','0','\0'},
	{0,0,0}
};

static const SceNpTitleSecret s_title_secret = {
	{
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	}
};

#define NUM_AGE_COUNTRIES 2
static const SCryLobbyAgeRestrictionSetup::SCountry	s_age_countries[NUM_AGE_COUNTRIES] = {
	{ "gb", 16 },
	{ "us", 18 }
};
static const SCryLobbyAgeRestrictionSetup	s_age_restrictions = {
	NUM_AGE_COUNTRIES,
	s_age_countries	
};

#define NUM_ADVERTISED_LANGUAGES 2
static const uint32 s_NumLocalisedSessionAdvertisementLanguages = NUM_ADVERTISED_LANGUAGES;
static const char* s_LocalisedLanguages[NUM_ADVERTISED_LANGUAGES] = {
	"en",
	"en-GB"
};
static const char* s_LocalisedSessionAdvertisementName[NUM_ADVERTISED_LANGUAGES] = {
	"Session Name US",
	"Session Name GB",
};
static const char* s_LocalisedSessionAdvertisementStatus[NUM_ADVERTISED_LANGUAGES] = {
	"Session description status US",
	"Session description status GB",
};

static const char* s_SessionAdvertisementImage = 
	"/9j/4AAQSkZJRgABAQEAYABgAAD/4QBoRXhpZgAATU0AKgAAAAgABAEaAAUAAAABAAAAPgEbAAUAAAABAAAARgEoAAMAAAABAAIAAAExAAIAAAASAAAATgAAAAAAAABgAAAAAQAAAGAAAAABUGFpbn"	\
	"QuTkVUIHYzLjUuMTAA/9sAQwD//////////////////////////////////////////////////////////////////////////////////////9sAQwH/////////////////////////////////"	\
	"/////////////////////////////////////////////////////8AAEQgAEAAQAwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQ"	\
	"ECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeo"	\
	"qaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQEAwQHBQQEAAECdwABAgMRBAUhMQ"	\
	"YSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2"	\
	"t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/aAAwDAQACEQMRAD8AkooooA//2Q==";

#endif


#if CRY_PLATFORM_ORBIS

/* static */
const char* CGameBrowser::GetGameModeStringFromId(int32 id)
{
	char *strGameMode = NULL;
	switch(id)
	{
	case RICHPRESENCE_GAMEMODES_INSTANTACTION:
		strGameMode = "@ui_rules_InstantAction";
		break;

	case RICHPRESENCE_GAMEMODES_TEAMINSTANTACTION:
		strGameMode = "@ui_rules_TeamInstantAction";
		break;

	case RICHPRESENCE_GAMEMODES_ASSAULT:
		strGameMode = "@ui_rules_Assault";
		break;

	case RICHPRESENCE_GAMEMODES_CAPTURETHEFLAG:
		strGameMode = "@ui_rules_CaptureTheFlag";
		break;

	case RICHPRESENCE_GAMEMODES_CRASHSITE:
		strGameMode = "@ui_rules_CrashSite";
		break;

	case RICHPRESENCE_GAMEMODES_ALLORNOTHING:
		strGameMode = "@ui_rules_AllOrNothing";
		break;

	case RICHPRESENCE_GAMEMODES_BOMBTHEBASE:
		strGameMode = "@ui_rules_BombTheBase";
		break;

	default:
		CRY_ASSERT_MESSAGE(false, "Failed to find game rules rich presence string");
		break;	
	}

	return strGameMode;
}

/* static */
const char* CGameBrowser::GetMapStringFromId(int32 id)
{
	char *strMap = NULL;
	return strMap;
}

/* static */
void CGameBrowser::LocalisePresenceString(CryFixedStringT<MAX_PRESENCE_STRING_SIZE> &out, const char* stringId)
{
	out = CHUDUtils::LocalizeString( stringId );
}

/*static*/
void CGameBrowser::UnpackRecievedInGamePresenceString(CryFixedStringT<MAX_PRESENCE_STRING_SIZE> &out, const CryFixedStringT<MAX_PRESENCE_STRING_SIZE>& inString)
{
	out = inString;
}

/*static*/
void CGameBrowser::LocaliseInGamePresenceString(CryFixedStringT<MAX_PRESENCE_STRING_SIZE> &out, const char* stringId, const int32 gameModeId, const int32 mapId)
{
	out = CHUDUtils::LocalizeString("@mp_rp_gameplay", GetGameModeStringFromId(gameModeId), GetMapStringFromId(mapId));
}

/* static */
bool CGameBrowser::CreatePresenceString(CryFixedStringT<MAX_PRESENCE_STRING_SIZE> &out, SCryLobbyUserData *pData, uint32 numData)
{
	bool result = true;

	if(numData > 0)
	{
		// pData[0] indicates the type of rich presence we setting, i.e. frontend, lobby, in-game
		// additional pData's are parameters that can be passed into the rich presence string, only used for gameplay at the moment
		switch(pData[CRichPresence::eRPT_String].m_int32)
		{
		case RICHPRESENCE_FRONTEND:
			LocalisePresenceString(out, "@mp_rp_frontend");
			break;

		case RICHPRESENCE_LOBBY:
			LocalisePresenceString(out, "@mp_rp_lobby");
			break;

		case RICHPRESENCE_GAMEPLAY:
			if(numData == 3)
			{
				const int gameModeId = pData[CRichPresence::eRPT_Param1].m_int32;
				const int mapId = pData[CRichPresence::eRPT_Param2].m_int32;
				LocaliseInGamePresenceString( out, "@mp_rp_gameplay", gameModeId, mapId );
			}
#if !defined(_RELEASE)
			else
			{
				CRY_ASSERT_MESSAGE(numData == 3, "Invalid data passed for gameplay rich presence state");
				result = false;
			}
#endif
			break;

		case RICHPRESENCE_SINGLEPLAYER:
			LocalisePresenceString(out, "@mp_rp_singleplayer");
			break;

		case RICHPRESENCE_IDLE:
			LocalisePresenceString(out, "@mp_rp_idle");
			break;

		default:
			CRY_ASSERT_MESSAGE(false, "[RichPresence] unknown rich presence type given");
			result = false;
			break;
		}
	}
	else
	{
		CryLog("[RichPresence] Failed to set rich presence because numData was 0 or there was no hud");
		result = false;
	}

	return result;
}

#endif

void CGameBrowser::ConfigurationCallback(ECryLobbyService service, SConfigurationParams *requestedParams, uint32 paramCount)
{
	uint32 a;
	for (a=0;a<paramCount;++a)
	{
		switch (requestedParams[a].m_fourCCID)
		{
		case CLCC_LAN_USER_NAME:
			{
				uint32 userIndex = g_pGame->GetExclusiveControllerDeviceIndex();

				IPlatformOS *pPlatformOS = gEnv->pSystem->GetPlatformOS();
				IPlatformOS::TUserName tUserName = "";
				if(pPlatformOS)
				{
					pPlatformOS->UserGetName(userIndex, tUserName);
				}
			
				// this will null terminate for us if necessary	
				cry_strcpy(s_profileName, tUserName.c_str());
				int instance = gEnv->pSystem->GetApplicationInstance();
				if (instance>0)
				{
					size_t length = strlen(s_profileName);
					if (length + 3 < CRYLOBBY_USER_NAME_LENGTH)
					{
						s_profileName[length] = '(';
						s_profileName[length+1] = '0' + instance;
						s_profileName[length+2] = ')';
						s_profileName[length+3] = 0;
					}
				}

				requestedParams[a].m_pData = s_profileName;
			}
			break;

#if CRY_PLATFORM_ORBIS
		case CLCC_CRYLOBBY_PRESENCE_CONVERTER:
			{
				SCryLobbyPresenceConverter* pConverter = (SCryLobbyPresenceConverter*)requestedParams[a].m_pData;
				if (pConverter)
				{
					//-- Use the pConverter->m_numData data items in pConverter->m_pData to create a string in pConverter->m_pStringBuffer
					//-- Use the initial value of pConverter->sizeOfStringBuffer as a maximum string length allowed, but
					//-- update pConverter->sizeOfStringBuffer to the correct length when the string is filled in.
					//-- Set pConverter->sizeOfStringBuffer = 0 to invalidate bad data so it isn't sent to PSN.
					CryFixedStringT<MAX_PRESENCE_STRING_SIZE> strPresence;
					if(CreatePresenceString(strPresence, pConverter->m_pData, pConverter->m_numData))
					{
						CryLog("[RichPresence] Succeeded %s", strPresence.c_str());
						sprintf((char*)pConverter->m_pStringBuffer, "%s", strPresence.c_str());
						pConverter->m_sizeOfStringBuffer = strlen((char*)pConverter->m_pStringBuffer);
					}
					else
					{
						CryLog("[RichPresence] Failed to create rich presence string");
						pConverter->m_sizeOfStringBuffer = 0;
					}
				}
			}
			break;
#endif
			
#if CRY_PLATFORM_DURANGO
		case CLCC_LIVE_TITLE_ID:
			requestedParams[a].m_32 = DURANGO_LIVE_TITLE_ID;
			break;
		case CLCC_LIVE_SERVICE_CONFIG_ID:
			requestedParams[a].m_pData = DURANGO_LIVE_SERVICE_CONFIG_ID;
			break;
#endif

#if CRY_PLATFORM_ORBIS
		case CLCC_PSN_TITLE_ID:
			requestedParams[a].m_pData = (void*)&s_title_id;
			break;

		case CLCC_PSN_TITLE_SECRET:
			requestedParams[a].m_pData = (void*)&s_title_secret;
			break;

		case CLCC_PSN_AGE_LIMIT:
			requestedParams[a].m_pData = (void*)&s_age_restrictions;
			break;

		case CLCC_PSN_PLUS_TEST_REQUIRED:
			requestedParams[a].m_32 = FALSE;
			break;

		case CLCC_PSN_CREATE_SESSION_ADVERTISEMENT:
			{
				SCryLobbySessionAdvertisement* pAdvertisement = (SCryLobbySessionAdvertisement*)requestedParams[a].m_pData;
				if (pAdvertisement)
				{
					pAdvertisement->m_numLanguages = s_NumLocalisedSessionAdvertisementLanguages;
					pAdvertisement->m_pLocalisedLanguages = (char**)s_LocalisedLanguages;
					pAdvertisement->m_pLocalisedSessionNames = (char**)s_LocalisedSessionAdvertisementName;
					pAdvertisement->m_pLocalisedSessionStatus = (char**)s_LocalisedSessionAdvertisementStatus;
					pAdvertisement->m_pJPGImage = (void*)s_SessionAdvertisementImage;
					pAdvertisement->m_sizeofJPGImage = strlen(s_SessionAdvertisementImage);
					// pAdvertisement->m_pData already points to a buffer of maximum size. It just needs to be filled in correctly.
					// pAdvertisement->m_sizeofData currently set to maximum size. You should change this to be correct size of actual data.
					// eg:	SAdvertDataStruct* pData = (SAdvertDataStruct*)pAdvertisement->m_pData;
					//			pData->id = 12345;
					//			pAdvertisement->m_sizeofData = sizeof(SAdvertDataStruct);
					uint32* pRealmId = (uint32*)pAdvertisement->m_pData;
					*pRealmId = 12345;
					pAdvertisement->m_sizeofData = sizeof(uint32);
				}
			}
			break;

		case CLCC_PSN_UPDATE_SESSION_ADVERTISEMENT:
			{
				SCryLobbySessionAdvertisement* pAdvertisement = (SCryLobbySessionAdvertisement*)requestedParams[a].m_pData;
				if (pAdvertisement)
				{
					// If pAdvertisement->m_numLanguages == 0, no attempt to alter the advertisement language settings will be made.
					// If pAdvertisement->m_numLanguages > 0, the previous language strings will all be overwritten with new ones.
					// (All strings are replaced or removed if not specified again)
					pAdvertisement->m_numLanguages = s_NumLocalisedSessionAdvertisementLanguages;
					pAdvertisement->m_pLocalisedLanguages = (char**)s_LocalisedLanguages;
					pAdvertisement->m_pLocalisedSessionNames = (char**)s_LocalisedSessionAdvertisementName;
					pAdvertisement->m_pLocalisedSessionStatus = (char**)s_LocalisedSessionAdvertisementStatus;
					// If pAdvertisement->m_pJPGImage is non-null and pAdvertisement->m_sizeofJPGImage > 0, the JPG data for the advertisement will be overwritten with a new JPG
					pAdvertisement->m_pJPGImage = (void*)s_SessionAdvertisementImage;
					pAdvertisement->m_sizeofJPGImage = strlen(s_SessionAdvertisementImage);
					// pAdvertisement->m_pData already points to a buffer containing a copy of the data passed into CryMatchmaking::SessionSetAdvertisementData.
					// and pAdvertisement->m_sizeofData is set to the correct size.
					// There should be no need to alter the contents of m_pData in this configuration callback.
				}
			}
			break;

		case CLCC_PSN_INVITE_BODY_STRING:
			{
				SCryLobbyXMBString* pStringData = (SCryLobbyXMBString*)requestedParams[a].m_pData;
				sprintf((char*)pStringData->m_pStringBuffer, "This is a test invite message");
				pStringData->m_sizeOfStringBuffer = strlen((char*)pStringData->m_pStringBuffer);
			}
			break;
#endif

		case CLCC_CRYSTATS_ENCRYPTION_KEY:
			{
#if CRY_PLATFORM_ORBIS
				requestedParams[a].m_pData = (void*)"";
#else
				requestedParams[a].m_pData = (void*)"";
#endif
			}
			break;

		case CLCC_MATCHMAKING_SESSION_PASSWORD_MAX_LENGTH:
			requestedParams[a].m_8 = MATCHMAKING_SESSION_PASSWORD_MAX_LENGTH;
			break;

#if USE_STEAM
#if !defined(RELEASE)
		case CLCC_STEAM_APPID:
			requestedParams[a].m_32 = STEAM_APPID;
			break;
#endif // !defined(RELEASE)
#endif // USE_STEAM

		default:
			CRY_ASSERT_MESSAGE(0,"Unknown Configuration Parameter Requested!");
			break;
		}
	}
}

//-------------------------------------------------------------------------
// Returns the NAT type when set-up.
void CGameBrowser::GetNatTypeCallback(UCryLobbyEventData eventData, void *arg)
{
	SCryLobbyNatTypeData *natTypeData = eventData.pNatTypeData;
	if (natTypeData)
	{
		CGameBrowser* pGameBrowser = (CGameBrowser*) arg;
		pGameBrowser->SetNatType(natTypeData->m_curState);

		const char* natString = pGameBrowser->GetNatTypeString();
		CryLog( "natString=%s", natString);

#if !defined(_RELEASE)
		if(g_pGameCVars)
		{
			g_pGameCVars->net_nat_type->ForceSet(natString);
		}
#endif
		if(g_pGame)
		{
			NOTIFY_UILOBBY_MP(UpdateNatType());
		}
	}
}

//-------------------------------------------------------------------------
const ENatType CGameBrowser::GetNatType() const
{
	return m_NatType;
}

//-------------------------------------------------------------------------
const char * CGameBrowser::GetNatTypeString() const
{
	switch(m_NatType)
	{
	case eNT_Open:
		return "@ui_mp_nattype_open";
	case eNT_Moderate:
		return "@ui_mp_nattype_moderate";
	case eNT_Strict:
		return "@ui_mp_nattype_strict";
	default:
		return "@ui_mp_nattype_unknown";
	};
	return "";
}

//-------------------------------------------------------------------------
// Register the data in CryLobby.
void CGameBrowser::InitialiseCallback(ECryLobbyService service, ECryLobbyError error, void* arg)
{
	assert( error == eCLE_Success );

	if (error == eCLE_Success)
	{
		SCryLobbyUserData userData[eLDI_Num];

		userData[eLDI_Gamemode].m_id = LID_MATCHDATA_GAMEMODE;
		userData[eLDI_Gamemode].m_type = eCLUDT_Int32;
		userData[eLDI_Gamemode].m_int32 = 0;

		userData[eLDI_Version].m_id = LID_MATCHDATA_VERSION;
		userData[eLDI_Version].m_type = eCLUDT_Int32;
		userData[eLDI_Version].m_int32 = 0;

		userData[eLDI_Playlist].m_id = LID_MATCHDATA_PLAYLIST;
		userData[eLDI_Playlist].m_type = eCLUDT_Int32;
		userData[eLDI_Playlist].m_int32 = 0;

		userData[eLDI_Variant].m_id = LID_MATCHDATA_VARIANT;
		userData[eLDI_Variant].m_type = eCLUDT_Int32;
		userData[eLDI_Variant].m_int32 = 0;

		userData[eLDI_RequiredDLCs].m_id = LID_MATCHDATA_REQUIRED_DLCS;
		userData[eLDI_RequiredDLCs].m_type = eCLUDT_Int32;
		userData[eLDI_RequiredDLCs].m_int32 = 0;

#if GAMELOBBY_USE_COUNTRY_FILTERING
		userData[eLDI_Country].m_id = LID_MATCHDATA_COUNTRY;
		userData[eLDI_Country].m_type = eCLUDT_Int32;
		userData[eLDI_Country].m_int32 = 0;
#endif

		userData[eLDI_Language].m_id = LID_MATCHDATA_LANGUAGE;
		userData[eLDI_Language].m_type = eCLUDT_Int32;
		userData[eLDI_Language].m_int32 = 0;

		userData[eLDI_Map].m_id = LID_MATCHDATA_MAP;
		userData[eLDI_Map].m_type = eCLUDT_Int32;
		userData[eLDI_Map].m_int32 = 0;

		userData[eLDI_Skill].m_id = LID_MATCHDATA_SKILL;
		userData[eLDI_Skill].m_type = eCLUDT_Int32;
		userData[eLDI_Skill].m_int32 = 0;

		userData[eLDI_Active].m_id = LID_MATCHDATA_ACTIVE;
		userData[eLDI_Active].m_type = eCLUDT_Int32;
		userData[eLDI_Active].m_int32 = 0;

		// we only want to register the user data with the service here, not set the service itself, we
		// already do that during the Init call
		gEnv->pNetwork->GetLobby()->GetLobbyService(service)->GetMatchMaking()->SessionRegisterUserData(userData, eLDI_Num, 0, NULL, NULL);

#if !defined( DEDICATED_SERVER )
		if ( gEnv->IsDedicated() )
#endif
		{
			if ( service == eCLS_Online )
			{
				TUserCredentials        credentials;
				
				gEnv->pNetwork->GetLobby()->GetLobbyService( service )->GetSignIn()->SignInUser( 0, credentials, NULL, NULL, NULL );
			}
		}
	}
}

//-------------------------------------------------------------------------
// Process a search result.
void CGameBrowser::MatchmakingSessionSearchCallback(CryLobbyTaskID taskID, ECryLobbyError error, SCrySessionSearchResult* session, void* arg)
{
#if 0 // old frontend
	CFlashFrontEnd *menu = g_pGame->GetFlashMenu();
	CMPMenuHub *mpMenuHub = menu ? menu->GetMPMenu() : NULL;
#endif
	CGameBrowser* pGameBrowser = (CGameBrowser*) arg;

	if (error == eCLE_SuccessContinue || error == eCLE_Success)
	{
#if 0 // old frontend
		if(session && mpMenuHub && GameLobbyData::IsValidServer(session))
		{
			CUIServerList::SServerInfo si;
			si.m_hostName     = session->m_data.m_name;

			CRY_TODO(20, 5, 2010, "In the case where too many servers have been filtered out, start a new search query and merge the results with the existing ones");
			int requiredDLCs = GameLobbyData::GetSearchResultsData(session, LID_MATCHDATA_REQUIRED_DLCS);
			CryLog("Found server (%s), num data (%d): with DLC version %d", si.m_hostName.c_str(), session->m_data.m_numData, requiredDLCs);
			if (g_pGameCVars->g_ignoreDLCRequirements || CDLCManager::MeetsDLCRequirements(requiredDLCs, g_pGame->GetDLCManager()->GetSquadCommonDLCs()) || pGameBrowser->m_bFavouriteIdSearch)
			{
				si.m_numPlayers   = session->m_numFilledSlots;
				si.m_maxPlayers   = session->m_data.m_numPublicSlots;
				si.m_gameTypeName = GameLobbyData::GetGameRulesFromHash(GameLobbyData::GetSearchResultsData(session, LID_MATCHDATA_GAMEMODE));
				si.m_gameTypeDisplayName = si.m_gameTypeName.c_str();
				si.m_mapName = GameLobbyData::GetMapFromHash(GameLobbyData::GetSearchResultsData(session, LID_MATCHDATA_MAP));
				si.m_mapDisplayName = PathUtil::GetFileName(si.m_mapName.c_str()).c_str();
				si.m_friends = (session->m_numFriends>0);
				si.m_reqPassword = session->m_flags&eCSSRF_RequirePassword;

				uint32 variantId = GameLobbyData::GetSearchResultsData(session, LID_MATCHDATA_VARIANT);
				bool bCustomVariant = false;
				CPlaylistManager *pPlaylistManager = g_pGame->GetPlaylistManager();
				if (pPlaylistManager)
				{
					const SGameVariant* pVariant = pPlaylistManager->GetVariant(variantId);
					if (pVariant)
					{
						si.m_gameVariantName = pVariant->m_name.c_str();
						si.m_gameVariantDisplayName = CHUDUtils::LocalizeString(pVariant->m_localName.c_str());
					}

					bCustomVariant = (variantId == pPlaylistManager->GetCustomVariant());
				}

				// Get more readable map name and game type
				if (ILocalizationManager* pLocMgr = gEnv->pSystem->GetLocalizationManager())
				{
					si.m_mapDisplayName = CHUDUtils::LocalizeString(g_pGame->GetMappedLevelName(si.m_mapDisplayName.c_str()));

					CryFixedStringT<64> tmpString;
					tmpString.Format("ui_rules_%s", si.m_gameTypeName.c_str());
					SLocalizedInfoGame outInfo;
					if (pLocMgr->GetLocalizedInfoByKey(tmpString.c_str(), outInfo))
					{
						wstring wcharstr = outInfo.swTranslatedText;
						CryStringUtils::WStrToUTF8(wcharstr, si.m_gameTypeDisplayName);
					}
				}

				// FIXME :
				//   Make server id unique in some other way....
				si.m_serverId     = (int)session->m_id.get(); // for lack of unique ids deref the pointer location of the server. This is the id that gets sent to flash...
				si.m_sessionId    = session->m_id;
				si.m_ping         = session->m_ping;

				if (pGameBrowser->m_bFavouriteIdSearch)
				{
#if IMPLEMENT_PC_BLADES
					if (si.m_sessionFavouriteKeyId != INVALID_SESSION_FAVOURITE_ID)
					{
						for (uint32 i = 0; i < pGameBrowser->m_currentSearchFavouriteIdIndex; ++i)
						{
							if (si.m_sessionFavouriteKeyId == pGameBrowser->m_searchFavouriteIds[i])
							{
								g_pGame->GetGameServerLists()->ServerFound(si, pGameBrowser->m_currentFavouriteIdSearchType, si.m_sessionFavouriteKeyId);

								// Invalidate any current search favourite ids, as it has been found
								pGameBrowser->m_searchFavouriteIds[i] = INVALID_SESSION_FAVOURITE_ID;
								break;
							}
						}
					}
#endif
				}
				else
				{
					mpMenuHub->AddServer(si);
				}
			}
		}
#endif
	}
	else if (error != eCLE_SuccessUnreachable)
	{
		CryLogAlways("CGameBrowser search for sessions error %d", (int)error);
		CGameLobby::ShowErrorDialog(error, NULL, NULL, NULL);
	}

	if ((error != eCLE_SuccessContinue) && (error != eCLE_SuccessUnreachable))
	{
		CryLogAlways("CCGameBrowser::MatchmakingSessionSearchCallback DONE");

		pGameBrowser->FinishedSearch(true, !pGameBrowser->m_bFavouriteIdSearch); // FavouriteId might start another search after this one has finished
	}
}

//-------------------------------------------------------------------------
// static
void CGameBrowser::InitLobbyServiceType()
{
	// Setup the default lobby service. 
	ECryLobbyService defaultLobbyService = eCLS_LAN;

#if LEADERBOARD_PLATFORM
	if (gEnv->IsDedicated())
	{
		if ( g_pGameCVars && (g_pGameCVars->g_useOnlineServiceForDedicated))
		{
			defaultLobbyService = eCLS_Online;
		}
	}
	else
	{
		defaultLobbyService = eCLS_Online;									// We support leaderboards so must use online by default to ensure stats are posted correctly
	}
#endif

#if !defined(_RELEASE) || defined(PERFORMANCE_BUILD)
	if (g_pGameCVars && g_pGameCVars->net_initLobbyServiceToLan)
	{
		defaultLobbyService = eCLS_LAN;
	}
#endif

	CGameLobby::SetLobbyService(defaultLobbyService);
}

#if IMPLEMENT_PC_BLADES
void CGameBrowser::StartFavouriteIdSearch( const CGameServerLists::EGameServerLists serverList, uint32 *pFavouriteIds, uint32 numFavouriteIds )
{
	CRY_ASSERT(numFavouriteIds <= CGameServerLists::k_maxServersStoredInList);
	numFavouriteIds = MIN(numFavouriteIds, CGameServerLists::k_maxServersStoredInList);
	if (numFavouriteIds <= CGameServerLists::k_maxServersStoredInList)
	{
		memset(m_searchFavouriteIds, INVALID_SESSION_FAVOURITE_ID, sizeof(m_searchFavouriteIds));

		for (uint32 i=0; i<numFavouriteIds; ++i)
		{
			m_searchFavouriteIds[i] = pFavouriteIds[i];
		}

		m_currentSearchFavouriteIdIndex = 0;
		m_numSearchFavouriteIds = numFavouriteIds;

		if (!CanStartSearch())
		{
			CryLog("[UI] Delayed Searching for favId sessions");
			m_delayedSearchType = eDST_FavouriteId;

			NOTIFY_UILOBBY_MP(SearchStarted());
		}
		else
		{
			if (DoFavouriteIdSearch())
			{
				m_currentFavouriteIdSearchType = serverList;

				NOTIFY_UILOBBY_MP(SearchStarted());
			}
		}
	}
}
#endif

//-------------------------------------------------------------------------
bool CGameBrowser::DoFavouriteIdSearch()
{
	CryLog("[UI] DoFavouriteIdSearch");

	bool bResult = false;

	return bResult;
}
