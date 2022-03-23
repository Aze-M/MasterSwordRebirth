//
// FuzzNet Data Handler - Load, Save, Update, Create characters
// Generally we want to load all three characters when player X joins the server
// Allow the player to easily switch between and use these characters.
//

#include "rapidjson/document_safe.h"
#include "base64/base64.h"
#include "msdllheaders.h"
#include "player.h"
#include "global.h"

#undef vector
#undef min
#undef max

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

#include "httprequesthandler.h"
#include "fndatahandler.h"

using namespace rapidjson;

enum RequestCommand
{
	FN_REQ_LOAD = 0,
	FN_REQ_CREATE,
	FN_REQ_UPDATE,
	FN_REQ_DELETE,
};

enum RequestResult
{
	FN_RES_NA = 0,
	FN_RES_OK,
	FN_RES_ERR,
};

struct FnRequestData
{
public:
	FnRequestData(int command, unsigned long long steamID, int slot, const char* url)
	{
		guid[0] = 0;
		data = NULL;
		result = FN_RES_NA;
		this->steamID = steamID;
		this->command = command;
		this->slot = slot;
		this->size = 0;
		strncpy(this->url, url, REQUEST_URL_SIZE);
	}

	~FnRequestData()
	{
		delete[] data;
	}

	int command;
	int slot;
	int size;
	unsigned long long steamID;
	char url[REQUEST_URL_SIZE];
	char guid[MSSTRING_SIZE];
	char* data;
	std::atomic<int> result;

private:
	FnRequestData(const FnRequestData& data);
};

static std::atomic<bool> g_bShouldShutdownFn(false);
static std::vector<FnRequestData*> g_vRequestData;
static std::vector<FnRequestData*> g_vIntermediateData;
static std::mutex mutex;
static std::condition_variable cv;
static float g_fThinkTime = 0.0f;

static bool IsSlotValid(int slot) { return ((slot >= 0) && (slot < MAX_CHARSLOTS)); }

static const char* GetFnUrl(char* fmt, ...)
{
	static char requestUrl[REQUEST_URL_SIZE], string[REQUEST_URL_SIZE];

	va_list argptr;
	va_start(argptr, fmt);
	vsnprintf(string, sizeof(string), fmt, argptr);
	va_end(argptr);

	_snprintf(requestUrl, REQUEST_URL_SIZE, "%s/", CVAR_GET_STRING("ms_central_addr"));
	strncat(requestUrl, string, REQUEST_URL_SIZE - strlen(requestUrl) - 1);

	return requestUrl;
}

// Load single char details.
static bool LoadCharacter(FnRequestData* req, const JSONValue& val)
{
	req->size = val["size"].GetInt();
	strncpy(req->guid, val["id"].GetString(), MSSTRING_SIZE);
	req->data = new char[req->size];
	memcpy(req->data, (char*)base64_decode(val["data"].GetString()).c_str(), req->size);
	req->result = FN_RES_OK;
	return true;
}

// Handle a char request.
static void HandleRequest(FnRequestData* req)
{
	if (req == NULL) return;

	req->result = FN_RES_ERR;
	JSONDocument* pDoc = NULL;
	const bool bIsUpdate = (req->command == FN_REQ_UPDATE);

	switch (req->command)
	{

	case FN_REQ_LOAD:
	{
		pDoc = HTTPRequestHandler::GetRequestAsJson(req->url);
		if (pDoc == NULL)
			return;

		const JSONDocument& doc = *pDoc;
		for (const JSONValue& val : doc["data"].GetArray()) // Iterate through the characters returned, if any.
		{
			if (LoadCharacter(req, val))
				break;
		}

		break;
	}

	case FN_REQ_CREATE:
	case FN_REQ_UPDATE:
	{
		char steamID64String[MSSTRING_SIZE];
		_snprintf(steamID64String, MSSTRING_SIZE, "%llu", req->steamID);

		StringBuffer s;
		Writer<StringBuffer> writer(s);

		writer.StartObject();

		writer.Key("steamid");
		writer.String(steamID64String);

		writer.Key("slot");
		writer.Int(req->slot);

		writer.Key("size");
		writer.Int(req->size);

		writer.Key("data");
		writer.String(base64_encode((const unsigned char*)req->data, req->size).c_str());

		writer.EndObject();

		if (bIsUpdate)
		{
			req->result = FN_RES_OK;
			HTTPRequestHandler::PutRequest(req->url, s.GetString());
			break;
		}

		pDoc = HTTPRequestHandler::PostRequestAsJson(req->url, s.GetString());
		if (pDoc == NULL)
			return;

		strncpy(req->guid, (*pDoc)["data"]["id"].GetString(), MSSTRING_SIZE);
		req->result = FN_RES_OK;
		break;
	}

	case FN_REQ_DELETE:
		HTTPRequestHandler::DeleteRequest(req->url);
		req->result = FN_RES_OK;
		break;

	}

	delete pDoc;
}

static void Worker(void)
{
	while (1)
	{
		std::unique_lock<std::mutex> lck(mutex);
		while ((g_vRequestData.size() == 0) && (g_bShouldShutdownFn == false))
			cv.wait(lck);

		if (g_bShouldShutdownFn)
			break;

		for (auto* pRequest : g_vRequestData)
		{
			if (pRequest->result != FN_RES_NA)
				continue;

			HandleRequest(pRequest);
		}
	}
}

void FnDataHandler::Initialize(void)
{
	g_bShouldShutdownFn = false;
	std::thread worker(Worker);
	worker.detach();
}

void FnDataHandler::Destroy(void)
{
	g_bShouldShutdownFn = true;
	cv.notify_all();
}

void FnDataHandler::Reset(void)
{
	cv.notify_all();

	// Wait for any remaining items.
	do
	{
		Think(true);
	} while (g_vRequestData.size());

	g_fThinkTime = 0.0f;
}

void FnDataHandler::Think(bool bNoCallback)
{
	if (!bNoCallback)
	{
		if (gpGlobals->time <= g_fThinkTime)
			return;

		g_fThinkTime = (gpGlobals->time + 0.025f);
	}

	std::unique_lock<std::mutex> lck(mutex, std::defer_lock);

	if (lck.try_lock())
	{
		for (int i = (g_vRequestData.size() - 1); i >= 0; i--)
		{
			const FnRequestData* req = g_vRequestData[i];
			if (req->result == FN_RES_NA) continue;

			CBasePlayer* pPlayer = (bNoCallback ? NULL : UTIL_PlayerBySteamID(req->steamID));
			if (pPlayer)
			{
				charinfo_t& CharInfo = pPlayer->m_CharInfo[req->slot];

				switch (req->command)
				{

				case FN_REQ_LOAD:
				case FN_REQ_CREATE:
				{
					if (req->result == FN_RES_OK)
					{
						strncpy(CharInfo.Guid, req->guid, MSSTRING_SIZE);
						CharInfo.AssignChar(req->slot, LOC_CENTRAL, (char*)req->data, req->size, pPlayer);
					}
					else
					{
						CharInfo.Index = req->slot;
						CharInfo.Location = LOC_CENTRAL;
						CharInfo.Status = CDS_NOTFOUND;
					}

					CharInfo.m_CachedStatus = CDS_UNLOADED; // force an update!
					break;
				}

				case FN_REQ_DELETE:
				{
					CharInfo.Status = CDS_NOTFOUND;
					CharInfo.m_CachedStatus = CDS_UNLOADED; // force an update!
					break;
				}

				}
			}

			g_vRequestData.erase(g_vRequestData.begin() + i);
			delete req;
		}

		// Add new requests
		if (g_vIntermediateData.size() > 0)
		{
			for (auto* pData : g_vIntermediateData)
				g_vRequestData.push_back(pData);

			g_vIntermediateData.clear();
			cv.notify_all();
		}
	}
}

bool FnDataHandler::IsEnabled(void)
{
	return (MSGlobals::CentralEnabled && !MSGlobals::IsLanGame && MSGlobals::ServerSideChar);
}

// Load all characters!
void FnDataHandler::LoadCharacter(CBasePlayer* pPlayer)
{
	if ((pPlayer == NULL) || (pPlayer->steamID64 == 0ULL)) return;

	for (int i = 0; i < MAX_CHARSLOTS; i++)
	{
		if (pPlayer->m_CharInfo[i].Status == CDS_LOADING)
			continue;

		pPlayer->m_CharInfo[i].m_CachedStatus = CDS_UNLOADED;
		pPlayer->m_CharInfo[i].Status = CDS_LOADING;
		g_vIntermediateData.push_back(new FnRequestData(FN_REQ_LOAD, pPlayer->steamID64, i, GetFnUrl("api/v1/character/%llu/%i", pPlayer->steamID64, i)));
	}
}

// Load a specific character!
void FnDataHandler::LoadCharacter(CBasePlayer* pPlayer, int slot)
{
	if ((pPlayer == NULL) || (pPlayer->steamID64 == 0ULL) || !IsSlotValid(slot) || (pPlayer->m_CharInfo[slot].Status == CDS_LOADING))
		return;

	pPlayer->m_CharInfo[slot].m_CachedStatus = CDS_UNLOADED;
	pPlayer->m_CharInfo[slot].Status = CDS_LOADING;
	g_vIntermediateData.push_back(new FnRequestData(FN_REQ_LOAD, pPlayer->steamID64, slot, GetFnUrl("api/v1/character/%llu/%i", pPlayer->steamID64, slot)));
}

// Create or Update character.
void FnDataHandler::CreateOrUpdateCharacter(CBasePlayer* pPlayer, int slot, const char* data, int size, bool bIsUpdate)
{
	if ((pPlayer == NULL) || (pPlayer->steamID64 == 0ULL) || (data == NULL) || (size <= 0) || !IsSlotValid(slot)) return; // Quick validation - steamId is vital.

	if (bIsUpdate && (pPlayer->m_CharacterState == CHARSTATE_UNLOADED)) return; // You cannot update your char (save) if there is no char loaded.

	FnRequestData* req = new FnRequestData(
		bIsUpdate ? FN_REQ_UPDATE : FN_REQ_CREATE,
		pPlayer->steamID64,
		slot,
		bIsUpdate ? GetFnUrl("api/v1/character/%s", pPlayer->m_CharInfo[slot].Guid) : GetFnUrl("api/v1/character/")
	);
	req->data = new char[size];
	req->size = size;
	memcpy(req->data, data, size);
	g_vIntermediateData.push_back(req);

	if (!bIsUpdate)
	{
		pPlayer->m_CharInfo[slot].m_CachedStatus = CDS_UNLOADED;
		pPlayer->m_CharInfo[slot].Status = CDS_LOADING;
	}
}

void FnDataHandler::DeleteCharacter(CBasePlayer* pPlayer, int slot)
{
	if ((pPlayer == NULL) || (pPlayer->steamID64 == 0ULL) || !IsSlotValid(slot)) return;

	pPlayer->m_CharInfo[slot].m_CachedStatus = CDS_UNLOADED;
	pPlayer->m_CharInfo[slot].Status = CDS_LOADING;
	g_vIntermediateData.push_back(new FnRequestData(FN_REQ_DELETE, pPlayer->steamID64, slot, GetFnUrl("api/v1/character/%s", pPlayer->m_CharInfo[slot].Guid)));
}

// We store 64-bit SteamIDs, convert from old 32-bit string based ID to 64-bit numeric.
// See https://developer.valvesoftware.com/wiki/SteamID for help.
// EX input: STEAM_0:0:7019991 = STEAM_X:Y:Z
// Formula: 
// V = 76561197960265728
// ID = Z*2+V+Y
unsigned long long FnDataHandler::GetSteamID64(const char* id)
{
	if (!id || !id[0]) return 0ULL;

	char pchSteamID[32];
	strncpy(pchSteamID, id, sizeof(pchSteamID));

	const char* pStart = pchSteamID;
	size_t size = strlen(pchSteamID);

	for (size_t i = 0; i < size; i++)
	{
		if (pchSteamID[i] == ':') // Skip the first part.
		{
			pStart += (i + 1);
			strncpy(pchSteamID, pStart, sizeof(pchSteamID)); // Removes STEAM_X:, WE ONLY CARE ABOUT Y:Z part!
			break;
		}
	}

	char pchArg1[16], pchArg2[16];
	pStart = pchSteamID;
	size = strlen(pchSteamID);

	for (size_t i = 0; i < size; i++)
	{
		if (pchSteamID[i] == ':') // split
		{
			strncpy(pchArg1, pStart, sizeof(pchArg1));
			pchArg1[i] = '\0';

			pStart += (i + 1);
			strncpy(pchArg2, pStart, sizeof(pchArg2));
			break;
		}
	}

	return (atoi(pchArg1) + 2ULL * atoi(pchArg2) + 76561197960265728ULL);
}