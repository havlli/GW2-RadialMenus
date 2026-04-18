///----------------------------------------------------------------------------------------------------
/// Copyright (c) Raidcore.GG - All rights reserved.
///
/// Name         :  Debug.cpp
/// Description  :  Implementation of the diagnostics panel.
/// Authors      :  Fork addition
///----------------------------------------------------------------------------------------------------

#include "Debug.h"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "nlohmann/json.hpp"
using json = nlohmann::json;

#include "RTAPI/RTAPI.hpp"

#include "Shared.h"
#include "Util.h"

namespace
{
	constexpr size_t LOG_CAPACITY = 256;
	constexpr size_t RECORDING_CAPACITY = 6000;     // ~5 min @ 20 Hz
	constexpr unsigned RECORDING_INTERVAL_MS = 50;  // 20 Hz
	constexpr size_t SNAPSHOT_SCHEMA_VERSION = 1;

	struct LogEntry
	{
		unsigned long long TimestampMs; // since Init
		std::string        Category;
		std::string        Message;
	};

	struct StoredSnapshot
	{
		unsigned long long TimestampMs;
		std::string        Label;
		std::string        Json;       // pre-serialised so it can be dumped back fast
	};

	HWND g_RegisteredHwnd = nullptr;

	std::atomic<unsigned long long> g_StartTimeMs{ 0 };

	/* raw input */
	std::atomic<unsigned long long> g_RawInputCount{ 0 };
	std::atomic<long long>          g_RawAccumX{ 0 };
	std::atomic<long long>          g_RawAccumY{ 0 };
	std::atomic<long>               g_LastRawDx{ 0 };
	std::atomic<long>               g_LastRawDy{ 0 };
	std::atomic<unsigned long long> g_LastRawInputTimeMs{ 0 };

	/* per-message WndProc counters — a small curated set */
	std::mutex                                  g_WndMutex;
	std::map<UINT, unsigned long long>          g_WndCounts;

	/* ring buffer */
	std::mutex                                  g_LogMutex;
	std::deque<LogEntry>                        g_Log;

	bool g_Paused = false;

	/* captured snapshots list (manual) */
	std::mutex                                  g_SnapshotsMutex;
	std::vector<StoredSnapshot>                 g_Snapshots;
	std::string                                 g_LastExportPath;

	/* recording (continuous sampling at RECORDING_INTERVAL_MS) */
	std::atomic<bool>                           g_RecordingOn{ false };
	std::mutex                                  g_RecordingMutex;
	std::deque<json>                            g_RecordingSamples;
	unsigned long long                          g_LastSampleMs = 0;
	std::string                                 g_LastRecordingPath;

	unsigned long long NowMs()
	{
		unsigned long long start = g_StartTimeMs.load();
		unsigned long long now = Time::GetTimestampMillis();
		if (start == 0) { return 0; }
		return now - start;
	}

	void AppendLog(const char* aCategory, const std::string& aMessage)
	{
		if (g_Paused) { return; }

		LogEntry e;
		e.TimestampMs = NowMs();
		e.Category = aCategory ? aCategory : "";
		e.Message = aMessage;

		const std::lock_guard<std::mutex> lock(g_LogMutex);
		g_Log.push_back(std::move(e));
		while (g_Log.size() > LOG_CAPACITY) { g_Log.pop_front(); }
	}

	std::string FormatHms(unsigned long long aMs)
	{
		unsigned long long total_s = aMs / 1000;
		unsigned long long ms = aMs % 1000;
		unsigned long long s = total_s % 60;
		unsigned long long m = (total_s / 60) % 60;
		unsigned long long h = total_s / 3600;
		char buf[32];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02llu:%02llu:%02llu.%03llu", h, m, s, ms);
		return buf;
	}

	/* ---------------- enum stringifiers ---------------- */

	const char* MsgName(UINT m)
	{
		switch (m)
		{
			case WM_KEYDOWN:         return "WM_KEYDOWN";
			case WM_KEYUP:           return "WM_KEYUP";
			case WM_LBUTTONDOWN:     return "WM_LBUTTONDOWN";
			case WM_LBUTTONUP:       return "WM_LBUTTONUP";
			case WM_LBUTTONDBLCLK:   return "WM_LBUTTONDBLCLK";
			case WM_RBUTTONDOWN:     return "WM_RBUTTONDOWN";
			case WM_RBUTTONUP:       return "WM_RBUTTONUP";
			case WM_RBUTTONDBLCLK:   return "WM_RBUTTONDBLCLK";
			case WM_MOUSEMOVE:       return "WM_MOUSEMOVE";
			case WM_MOUSEWHEEL:      return "WM_MOUSEWHEEL";
			case WM_INPUT:           return "WM_INPUT";
			case WM_SETCURSOR:       return "WM_SETCURSOR";
			case WM_ACTIVATE:        return "WM_ACTIVATE";
			case WM_ACTIVATEAPP:     return "WM_ACTIVATEAPP";
			case WM_SETFOCUS:        return "WM_SETFOCUS";
			case WM_KILLFOCUS:       return "WM_KILLFOCUS";
		}
		return nullptr;
	}

	const char* MountIndexName(Mumble::EMountIndex m)
	{
		switch (m)
		{
			case Mumble::EMountIndex::None:         return "None";
			case Mumble::EMountIndex::Jackal:       return "Jackal";
			case Mumble::EMountIndex::Griffon:      return "Griffon";
			case Mumble::EMountIndex::Springer:     return "Springer";
			case Mumble::EMountIndex::Skimmer:      return "Skimmer";
			case Mumble::EMountIndex::Raptor:       return "Raptor";
			case Mumble::EMountIndex::RollerBeetle: return "Roller Beetle";
			case Mumble::EMountIndex::Warclaw:      return "Warclaw";
			case Mumble::EMountIndex::Skyscale:     return "Skyscale";
			case Mumble::EMountIndex::Skiff:        return "Skiff";
			case Mumble::EMountIndex::SiegeTurtle:  return "Siege Turtle";
		}
		return "?";
	}

	const char* MapTypeName(Mumble::EMapType t)
	{
		switch (t)
		{
			case Mumble::EMapType::AutoRedirect:             return "AutoRedirect";
			case Mumble::EMapType::CharacterCreation:        return "CharacterCreation";
			case Mumble::EMapType::PvP:                      return "PvP";
			case Mumble::EMapType::GvG:                      return "GvG";
			case Mumble::EMapType::Instance:                 return "Instance";
			case Mumble::EMapType::Public:                   return "Public";
			case Mumble::EMapType::Tournament:               return "Tournament";
			case Mumble::EMapType::Tutorial:                 return "Tutorial";
			case Mumble::EMapType::UserTournament:           return "UserTournament";
			case Mumble::EMapType::WvW_EternalBattlegrounds: return "WvW_EternalBattlegrounds";
			case Mumble::EMapType::WvW_BlueBorderlands:      return "WvW_BlueBorderlands";
			case Mumble::EMapType::WvW_GreenBorderlands:     return "WvW_GreenBorderlands";
			case Mumble::EMapType::WvW_RedBorderlands:       return "WvW_RedBorderlands";
			case Mumble::EMapType::WVW_FortunesVale:         return "WvW_FortunesVale";
			case Mumble::EMapType::WvW_ObsidianSanctum:      return "WvW_ObsidianSanctum";
			case Mumble::EMapType::WvW_EdgeOfTheMists:       return "WvW_EdgeOfTheMists";
			case Mumble::EMapType::Public_Mini:              return "Public_Mini";
			case Mumble::EMapType::BigBattle:                return "BigBattle";
			case Mumble::EMapType::WvW_Lounge:               return "WvW_Lounge";
		}
		return "?";
	}

	const char* ProfessionName(Mumble::EProfession p)
	{
		switch (p)
		{
			case Mumble::EProfession::None:         return "None";
			case Mumble::EProfession::Guardian:     return "Guardian";
			case Mumble::EProfession::Warrior:      return "Warrior";
			case Mumble::EProfession::Engineer:     return "Engineer";
			case Mumble::EProfession::Ranger:       return "Ranger";
			case Mumble::EProfession::Thief:        return "Thief";
			case Mumble::EProfession::Elementalist: return "Elementalist";
			case Mumble::EProfession::Mesmer:       return "Mesmer";
			case Mumble::EProfession::Necromancer:  return "Necromancer";
			case Mumble::EProfession::Revenant:     return "Revenant";
		}
		return "?";
	}

	const char* RaceName(Mumble::ERace r)
	{
		switch (r)
		{
			case Mumble::ERace::Asura:   return "Asura";
			case Mumble::ERace::Charr:   return "Charr";
			case Mumble::ERace::Human:   return "Human";
			case Mumble::ERace::Norn:    return "Norn";
			case Mumble::ERace::Sylvari: return "Sylvari";
		}
		return "?";
	}

	const char* RtapiGameStateName(RTAPI::EGameState s)
	{
		switch (s)
		{
			case RTAPI::EGameState::CharacterSelection: return "CharacterSelection";
			case RTAPI::EGameState::CharacterCreation:  return "CharacterCreation";
			case RTAPI::EGameState::Cinematic:          return "Cinematic";
			case RTAPI::EGameState::LoadingScreen:      return "LoadingScreen";
			case RTAPI::EGameState::Gameplay:           return "Gameplay";
		}
		return "?";
	}

	const char* RtapiLanguageName(RTAPI::EGameLanguage l)
	{
		switch (l)
		{
			case RTAPI::EGameLanguage::English: return "English";
			case RTAPI::EGameLanguage::Korean:  return "Korean";
			case RTAPI::EGameLanguage::French:  return "French";
			case RTAPI::EGameLanguage::German:  return "German";
			case RTAPI::EGameLanguage::Spanish: return "Spanish";
			case RTAPI::EGameLanguage::Chinese: return "Chinese";
		}
		return "?";
	}

	const char* RtapiTimeOfDayName(RTAPI::ETimeOfDay t)
	{
		switch (t)
		{
			case RTAPI::ETimeOfDay::Dawn:  return "Dawn";
			case RTAPI::ETimeOfDay::Day:   return "Day";
			case RTAPI::ETimeOfDay::Dusk:  return "Dusk";
			case RTAPI::ETimeOfDay::Night: return "Night";
		}
		return "?";
	}

	const char* RtapiGroupTypeName(RTAPI::EGroupType g)
	{
		switch (g)
		{
			case RTAPI::EGroupType::None:      return "None";
			case RTAPI::EGroupType::Party:     return "Party";
			case RTAPI::EGroupType::RaidSquad: return "RaidSquad";
			case RTAPI::EGroupType::Squad:     return "Squad";
		}
		return "?";
	}

	/* List of game binds we want to expose on the probe. Keeps the panel
	   focused; users can add more as needed. */
	struct ProbeEntry { EGameBinds Bind; const char* Label; };
	const ProbeEntry kProbeBinds[] = {
		{ EGameBinds_CameraActionMode,        "CameraActionMode" },
		{ EGameBinds_CameraActionModeDisable, "CameraActionModeDisable" },
		{ EGameBinds_CameraFree,              "CameraFree" },
		{ EGameBinds_SpumoniToggle,           "SpumoniToggle" },
		{ EGameBinds_SpumoniMAM01,            "SpumoniMAM01 (Raptor)" },
		{ EGameBinds_SpumoniMAM02,            "SpumoniMAM02 (Springer)" },
		{ EGameBinds_SpumoniMAM03,            "SpumoniMAM03 (Skimmer)" },
		{ EGameBinds_SpumoniMAM04,            "SpumoniMAM04 (Jackal)" },
		{ EGameBinds_SpumoniMAM05,            "SpumoniMAM05 (Griffon)" },
		{ EGameBinds_SpumoniMAM06,            "SpumoniMAM06 (Roller Beetle)" },
		{ EGameBinds_SpumoniMAM07,            "SpumoniMAM07 (Warclaw)" },
		{ EGameBinds_SpumoniMAM08,            "SpumoniMAM08 (Skyscale)" },
		{ EGameBinds_SpumoniMAM09,            "SpumoniMAM09 (Siege Turtle)" },
		{ EGameBinds_MiscInteract,            "MiscInteract" },
		{ EGameBinds_MoveDodge,               "MoveDodge" },
	};

	/* ---------------- snapshot builders ---------------- */

	/* Heuristic: guess the state from live metrics so users don't have to
	   label every snapshot. Covers the combinations that matter for the
	   current AC debugging. Ambiguous cases get flagged. */
	std::string InferState()
	{
		CURSORINFO ci{};
		ci.cbSize = sizeof(ci);
		bool cursorOk = GetCursorInfo(&ci) == TRUE;
		bool hidden = cursorOk && !(ci.flags & CURSOR_SHOWING);

		RECT clip{};
		bool clipPinned = false;
		if (GetClipCursor(&clip))
		{
			LONG cw = clip.right - clip.left;
			LONG ch = clip.bottom - clip.top;
			clipPinned = (cw <= 2 && ch <= 2);
		}

		bool rtapiAC = RTAPIData ? (bool)RTAPIData->IsActionCamera : false;
		bool rtapiPresent = (RTAPIData != nullptr);

		std::string s;
		if (hidden && (rtapiAC || !rtapiPresent)) { s = "cursor-hidden (likely Action-Cam or RMB-pan)"; }
		else if (hidden)                          { s = "cursor-hidden, RTAPI says NOT AC (likely right-click pan)"; }
		else                                       { s = "cursor-visible (normal)"; }

		if (clipPinned) { s += " + ClipCursor=1x1 (Nexus MouseResetFix active)"; }
		if (rtapiPresent && rtapiAC && !hidden) { s += " [ANOMALY: RTAPI says AC but cursor is visible]"; }
		if (rtapiPresent && !rtapiAC && hidden) { s += " [RTAPI disagrees: not AC]"; }

		return s;
	}

	json BuildSnapshotJson(const std::string& aLabel)
	{
		json j;
		j["schemaVersion"] = SNAPSHOT_SCHEMA_VERSION;
		j["capturedAtMs"] = NowMs();
		j["label"] = aLabel;
		j["inferredState"] = InferState();

		/* cursor */
		CURSORINFO ci{};
		ci.cbSize = sizeof(ci);
		BOOL ciOk = GetCursorInfo(&ci);
		j["cursor"]["getCursorInfo"]["ok"] = (bool)ciOk;
		if (ciOk)
		{
			j["cursor"]["getCursorInfo"]["flags"] = (unsigned)ci.flags;
			j["cursor"]["getCursorInfo"]["cursorShowing"]    = (bool)(ci.flags & CURSOR_SHOWING);
			j["cursor"]["getCursorInfo"]["cursorSuppressed"] = (bool)(ci.flags & CURSOR_SUPPRESSED);
			j["cursor"]["getCursorInfo"]["ptScreenPos"]      = { {"x", ci.ptScreenPos.x}, {"y", ci.ptScreenPos.y} };
			j["cursor"]["isCursorHidden"] = !(ci.flags & CURSOR_SHOWING);
		}
		POINT p{};
		BOOL pOk = GetCursorPos(&p);
		j["cursor"]["getCursorPos"] = { {"ok", (bool)pOk}, {"x", p.x}, {"y", p.y} };
		ImVec2 ip = ImGui::GetMousePos();
		j["cursor"]["imguiMousePos"] = { {"x", ip.x}, {"y", ip.y} };
		RECT clip{};
		BOOL gcOk = GetClipCursor(&clip);
		if (gcOk)
		{
			LONG cw = clip.right - clip.left;
			LONG ch = clip.bottom - clip.top;
			j["cursor"]["clipRect"] = {
				{"left", clip.left}, {"top", clip.top}, {"right", clip.right}, {"bottom", clip.bottom},
				{"w", cw}, {"h", ch}, {"pinned1x1", (cw <= 2 && ch <= 2)}
			};
		}

		/* action cam */
		j["actionCam"]["cameraActionModeBoundInNexus"] = (APIDefs != nullptr) && APIDefs->GameBinds.IsBound(EGameBinds_CameraActionMode);
		j["actionCam"]["rtapiPresent"]                  = (RTAPIData != nullptr);
		j["actionCam"]["rtapiIsActionCamera"]           = RTAPIData ? (bool)RTAPIData->IsActionCamera : false;

		/* raw input */
		unsigned long long last_raw = g_LastRawInputTimeMs.load();
		j["rawInput"]["registered"]      = (g_RegisteredHwnd != nullptr);
		j["rawInput"]["totalEvents"]     = g_RawInputCount.load();
		j["rawInput"]["lastDelta"]       = { {"dx", g_LastRawDx.load()}, {"dy", g_LastRawDy.load()} };
		j["rawInput"]["accumulated"]     = { {"x", g_RawAccumX.load()}, {"y", g_RawAccumY.load()} };
		j["rawInput"]["lastEventMs"]     = last_raw;
		j["rawInput"]["lastEventMsAgo"]  = (last_raw == 0) ? 0ULL : (NowMs() - last_raw);

		/* gameBinds probe */
		{
			json binds = json::array();
			for (const ProbeEntry& e : kProbeBinds)
			{
				binds.push_back({
					{"label", e.Label},
					{"id",    (int)e.Bind},
					{"bound", APIDefs ? APIDefs->GameBinds.IsBound(e.Bind) : false}
				});
			}
			j["gameBinds"] = std::move(binds);
		}

		/* wndproc counters */
		{
			json wc = json::object();
			const std::lock_guard<std::mutex> lock(g_WndMutex);
			for (const auto& [m, c] : g_WndCounts)
			{
				const char* name = MsgName(m);
				std::string key = name ? name : ("0x" + std::to_string(m));
				wc[key] = c;
			}
			j["wndProcCounts"] = std::move(wc);
		}

		/* mumble data */
		if (MumbleLink)
		{
			const Mumble::Data& d = *MumbleLink;
			const Mumble::Context& c = d.Context;
			j["mumble"]["present"]                = true;
			j["mumble"]["data"]["uiVersion"]      = d.UIVersion;
			j["mumble"]["data"]["uiTick"]         = d.UITick;
			j["mumble"]["data"]["avatarPosition"] = { d.AvatarPosition.X, d.AvatarPosition.Y, d.AvatarPosition.Z };
			j["mumble"]["data"]["avatarFront"]    = { d.AvatarFront.X, d.AvatarFront.Y, d.AvatarFront.Z };
			j["mumble"]["data"]["avatarTop"]      = { d.AvatarTop.X, d.AvatarTop.Y, d.AvatarTop.Z };
			j["mumble"]["data"]["cameraPosition"] = { d.CameraPosition.X, d.CameraPosition.Y, d.CameraPosition.Z };
			j["mumble"]["data"]["cameraFront"]    = { d.CameraFront.X, d.CameraFront.Y, d.CameraFront.Z };
			j["mumble"]["data"]["cameraTop"]      = { d.CameraTop.X, d.CameraTop.Y, d.CameraTop.Z };
			j["mumble"]["context"]["mapID"]                = c.MapID;
			j["mumble"]["context"]["mapType"]              = MapTypeName(c.MapType);
			j["mumble"]["context"]["mapTypeRaw"]           = (int)c.MapType;
			j["mumble"]["context"]["shardID"]              = c.ShardID;
			j["mumble"]["context"]["instanceID"]           = c.InstanceID;
			j["mumble"]["context"]["buildID"]              = c.BuildID;
			j["mumble"]["context"]["mountIndex"]           = MountIndexName(c.MountIndex);
			j["mumble"]["context"]["mountIndexRaw"]        = (int)c.MountIndex;
			j["mumble"]["context"]["processID"]            = c.ProcessID;
			j["mumble"]["context"]["isMapOpen"]            = (bool)c.IsMapOpen;
			j["mumble"]["context"]["isCompassTopRight"]    = (bool)c.IsCompassTopRight;
			j["mumble"]["context"]["isCompassRotating"]    = (bool)c.IsCompassRotating;
			j["mumble"]["context"]["isGameFocused"]        = (bool)c.IsGameFocused;
			j["mumble"]["context"]["isCompetitive"]        = (bool)c.IsCompetitive;
			j["mumble"]["context"]["isTextboxFocused"]     = (bool)c.IsTextboxFocused;
			j["mumble"]["context"]["isInCombat"]           = (bool)c.IsInCombat;
		}
		else
		{
			j["mumble"]["present"] = false;
		}

		/* mumble identity */
		if (MumbleIdentity)
		{
			const Mumble::Identity& id = *MumbleIdentity;
			j["mumble"]["identity"]["present"]        = true;
			j["mumble"]["identity"]["name"]           = id.Name;
			j["mumble"]["identity"]["profession"]     = ProfessionName(id.Profession);
			j["mumble"]["identity"]["specialization"] = id.Specialization;
			j["mumble"]["identity"]["race"]           = RaceName(id.Race);
			j["mumble"]["identity"]["mapID"]          = id.MapID;
			j["mumble"]["identity"]["worldID"]        = id.WorldID;
			j["mumble"]["identity"]["teamColorID"]    = id.TeamColorID;
			j["mumble"]["identity"]["isCommander"]    = id.IsCommander;
			j["mumble"]["identity"]["fov"]            = id.FOV;
			j["mumble"]["identity"]["uiSize"]         = (int)id.UISize;
		}
		else
		{
			j["mumble"]["identity"]["present"] = false;
		}

		/* nexusLink */
		if (NexusLink)
		{
			j["nexusLink"]["present"]        = true;
			j["nexusLink"]["width"]          = NexusLink->Width;
			j["nexusLink"]["height"]         = NexusLink->Height;
			j["nexusLink"]["scaling"]        = NexusLink->Scaling;
			j["nexusLink"]["isMoving"]       = NexusLink->IsMoving;
			j["nexusLink"]["isCameraMoving"] = NexusLink->IsCameraMoving;
			j["nexusLink"]["isGameplay"]     = NexusLink->IsGameplay;
		}
		else
		{
			j["nexusLink"]["present"] = false;
		}

		/* rtapi */
		if (RTAPIData)
		{
			const RTAPI::RealTimeData& r = *RTAPIData;
			j["rtapi"]["present"]          = true;
			j["rtapi"]["gameBuild"]        = r.GameBuild;
			j["rtapi"]["gameState"]        = RtapiGameStateName(r.GameState);
			j["rtapi"]["language"]         = RtapiLanguageName(r.Language);
			j["rtapi"]["timeOfDay"]        = RtapiTimeOfDayName(r.TimeOfDay);
			j["rtapi"]["mapID"]            = r.MapID;
			j["rtapi"]["mapTypeRaw"]       = (unsigned)r.MapType;
			j["rtapi"]["groupType"]        = RtapiGroupTypeName(r.GroupType);
			j["rtapi"]["groupMemberCount"] = r.GroupMemberCount;
			j["rtapi"]["accountName"]      = r.AccountName;
			j["rtapi"]["characterName"]    = r.CharacterName;
			j["rtapi"]["characterPosition"] = { r.CharacterPosition[0], r.CharacterPosition[1], r.CharacterPosition[2] };
			j["rtapi"]["characterFacing"]   = { r.CharacterFacing[0], r.CharacterFacing[1], r.CharacterFacing[2] };
			j["rtapi"]["profession"]          = r.Profession;
			j["rtapi"]["eliteSpecialization"] = r.EliteSpecialization;
			j["rtapi"]["mountIndex"]          = r.MountIndex;
			unsigned cs = (unsigned)r.CharacterState;
			j["rtapi"]["characterState"]["raw"]          = cs;
			j["rtapi"]["characterState"]["isAlive"]      = (bool)(cs & (unsigned)RTAPI::ECharacterState::IsAlive);
			j["rtapi"]["characterState"]["isDowned"]     = (bool)(cs & (unsigned)RTAPI::ECharacterState::IsDowned);
			j["rtapi"]["characterState"]["isInCombat"]   = (bool)(cs & (unsigned)RTAPI::ECharacterState::IsInCombat);
			j["rtapi"]["characterState"]["isSwimming"]   = (bool)(cs & (unsigned)RTAPI::ECharacterState::IsSwimming);
			j["rtapi"]["characterState"]["isUnderwater"] = (bool)(cs & (unsigned)RTAPI::ECharacterState::IsUnderwater);
			j["rtapi"]["characterState"]["isGliding"]    = (bool)(cs & (unsigned)RTAPI::ECharacterState::IsGliding);
			j["rtapi"]["characterState"]["isFlying"]     = (bool)(cs & (unsigned)RTAPI::ECharacterState::IsFlying);
			j["rtapi"]["cameraPosition"] = { r.CameraPosition[0], r.CameraPosition[1], r.CameraPosition[2] };
			j["rtapi"]["cameraFacing"]   = { r.CameraFacing[0], r.CameraFacing[1], r.CameraFacing[2] };
			j["rtapi"]["cameraFOV"]      = r.CameraFOV;
			j["rtapi"]["isActionCamera"] = (bool)r.IsActionCamera;
		}
		else
		{
			j["rtapi"]["present"] = false;
		}

		/* events (rolling ring) */
		{
			json events = json::array();
			const std::lock_guard<std::mutex> lock(g_LogMutex);
			for (const LogEntry& e : g_Log)
			{
				events.push_back({
					{"tMs",      e.TimestampMs},
					{"category", e.Category},
					{"message",  e.Message}
				});
			}
			j["events"] = std::move(events);
		}

		return j;
	}

	/* Slim per-frame sample used for recording. Serialised to JSON to avoid
	   drift against the snapshot format. */
	json BuildSampleJson()
	{
		json s;
		s["tMs"] = NowMs();

		CURSORINFO ci{};
		ci.cbSize = sizeof(ci);
		bool hidden = false;
		if (GetCursorInfo(&ci))
		{
			hidden = !(ci.flags & CURSOR_SHOWING);
			s["hidden"] = hidden;
			s["cursor"] = { ci.ptScreenPos.x, ci.ptScreenPos.y };
		}
		RECT clip{};
		if (GetClipCursor(&clip))
		{
			LONG cw = clip.right - clip.left;
			LONG ch = clip.bottom - clip.top;
			s["clip1x1"] = (cw <= 2 && ch <= 2);
		}
		s["wmInput"]   = g_RawInputCount.load();
		s["lastDelta"] = { g_LastRawDx.load(), g_LastRawDy.load() };
		ImVec2 ip = ImGui::GetMousePos();
		s["imguiMouse"] = { ip.x, ip.y };
		if (RTAPIData)
		{
			s["rtapiAC"]     = (bool)RTAPIData->IsActionCamera;
			s["rtapiCombat"] = (bool)((unsigned)RTAPIData->CharacterState & (unsigned)RTAPI::ECharacterState::IsInCombat);
		}
		if (MumbleLink)
		{
			s["combat"]   = (bool)MumbleLink->Context.IsInCombat;
			s["mountIdx"] = (int)MumbleLink->Context.MountIndex;
			s["mapID"]    = MumbleLink->Context.MapID;
		}
		if (NexusLink)
		{
			s["nexusCamMoving"] = NexusLink->IsCameraMoving;
			s["nexusIsGameplay"] = NexusLink->IsGameplay;
		}
		return s;
	}

	std::string BuildFilename(const char* aPrefix)
	{
		time_t now = time(nullptr);
		struct tm tm{};
		localtime_s(&tm, &now);
		char buf[64];
		strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
		std::string out = aPrefix;
		out.append("-");
		out.append(buf);
		out.append(".json");
		return out;
	}

	std::filesystem::path SnapshotDir()
	{
		std::filesystem::path dir = AddonDirectory / "debug";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		return dir;
	}
}

namespace Debug
{
	void Init(HWND aGameWindow)
	{
		if (g_StartTimeMs.load() == 0)
		{
			g_StartTimeMs.store(Time::GetTimestampMillis());
			AppendLog("Debug", "diagnostics initialised");
		}

		if (g_RegisteredHwnd == aGameWindow || aGameWindow == nullptr) { return; }

		RAWINPUTDEVICE rid{};
		rid.usUsagePage = 0x01; // HID generic desktop
		rid.usUsage = 0x02;     // mouse
		rid.dwFlags = 0;        // foreground only (we don't need background)
		rid.hwndTarget = aGameWindow;
		if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
		{
			g_RegisteredHwnd = aGameWindow;
			Log("RawInput", "registered for WM_INPUT on hwnd=%p", aGameWindow);
		}
		else
		{
			Log("RawInput", "RegisterRawInputDevices failed (err=%lu)", GetLastError());
		}
	}

	void Shutdown()
	{
		if (g_RegisteredHwnd)
		{
			RAWINPUTDEVICE rid{};
			rid.usUsagePage = 0x01;
			rid.usUsage = 0x02;
			rid.dwFlags = RIDEV_REMOVE;
			rid.hwndTarget = nullptr;
			RegisterRawInputDevices(&rid, 1, sizeof(rid));
			g_RegisteredHwnd = nullptr;
		}
	}

	UINT OnWndProc(HWND /*hWnd*/, UINT uMsg, WPARAM /*wParam*/, LPARAM lParam)
	{
		if (MsgName(uMsg) != nullptr)
		{
			const std::lock_guard<std::mutex> lock(g_WndMutex);
			g_WndCounts[uMsg]++;
		}

		if (uMsg == WM_INPUT)
		{
			UINT dwSize = 0;
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER)) == 0 && dwSize > 0 && dwSize <= sizeof(RAWINPUT))
			{
				RAWINPUT raw{};
				if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize)
				{
					if (raw.header.dwType == RIM_TYPEMOUSE)
					{
						LONG dx = raw.data.mouse.lLastX;
						LONG dy = raw.data.mouse.lLastY;
						g_RawInputCount.fetch_add(1);
						g_RawAccumX.fetch_add(dx);
						g_RawAccumY.fetch_add(dy);
						g_LastRawDx.store(dx);
						g_LastRawDy.store(dy);
						g_LastRawInputTimeMs.store(NowMs());
					}
				}
			}
		}

		return uMsg;
	}

	void Log(const char* aCategory, const char* aFormat, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, aFormat);
		_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, aFormat, args);
		va_end(args);
		AppendLog(aCategory, buf);
	}

	void OnRadialActivateEvaluated(const std::string& aRadialName, bool aWasActionCamActive, bool aLeftHeld, bool aRightHeld)
	{
		Log("Activate", "'%s' WasACActive=%d LMB=%d RMB=%d",
			aRadialName.c_str(), (int)aWasActionCamActive, (int)aLeftHeld, (int)aRightHeld);
	}

	void OnActionCamToggleIssued(bool aDirection)
	{
		Log("ACToggle", "issued %s (GameBinds.Press/Release CameraActionMode)", aDirection ? "OPENING" : "CLOSING");
	}

	void OnCursorWarpIssued(int aX, int aY, const char* aReason)
	{
		Log("CursorWarp", "to (%d,%d) reason=%s", aX, aY, aReason ? aReason : "?");
	}

	void OnRadialReleased(const std::string& aRadialName, int aSelectionMode, int aHoverIndex)
	{
		Log("Release", "'%s' mode=%d hoverIndex=%d", aRadialName.c_str(), aSelectionMode, aHoverIndex);
	}

	void Tick()
	{
		if (!g_RecordingOn.load()) { return; }

		unsigned long long now = NowMs();
		if (now - g_LastSampleMs < RECORDING_INTERVAL_MS) { return; }
		g_LastSampleMs = now;

		json s = BuildSampleJson();
		const std::lock_guard<std::mutex> lock(g_RecordingMutex);
		g_RecordingSamples.push_back(std::move(s));
		while (g_RecordingSamples.size() > RECORDING_CAPACITY) { g_RecordingSamples.pop_front(); }
	}

	void RenderPanel()
	{
		/* ---------------- header row ---------------- */

		ImGui::Text("Session time: %s", FormatHms(NowMs()).c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton(g_Paused ? "Resume logging" : "Pause logging"))
		{
			g_Paused = !g_Paused;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Clear log##eventlog"))
		{
			const std::lock_guard<std::mutex> lock(g_LogMutex);
			g_Log.clear();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy log"))
		{
			std::string out;
			{
				const std::lock_guard<std::mutex> lock(g_LogMutex);
				for (const LogEntry& e : g_Log)
				{
					char buf[64];
					_snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%s] ", FormatHms(e.TimestampMs).c_str());
					out.append(buf);
					out.append("[");
					out.append(e.Category);
					out.append("] ");
					out.append(e.Message);
					out.append("\r\n");
				}
			}
			ImGui::SetClipboardText(out.c_str());
		}

		ImGui::Separator();

		/* ---------------- snapshot + recording ---------------- */

		if (ImGui::CollapsingHeader("Snapshot & Recording", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextDisabled("Capture the full metric dump as JSON. Label each one so they're easy to tell apart when you paste them back.");

			/* label input */
			static char s_labelBuf[128] = "";
			ImGui::SetNextItemWidth(300);
			ImGui::InputTextWithHint("##snaplabel", "Label (e.g. 'AC off wheel closed')", s_labelBuf, sizeof(s_labelBuf));
			ImGui::SameLine();
			if (ImGui::Button("Add snapshot"))
			{
				std::string label = s_labelBuf[0] ? std::string(s_labelBuf) : std::string("(unlabelled)");
				json j = BuildSnapshotJson(label);
				StoredSnapshot ss;
				ss.TimestampMs = NowMs();
				ss.Label = label;
				ss.Json = j.dump();
				{
					const std::lock_guard<std::mutex> lock(g_SnapshotsMutex);
					g_Snapshots.push_back(std::move(ss));
				}
				Log("Snapshot", "captured '%s' (inferred: %s)", label.c_str(), j.value("inferredState", "?").c_str());
				s_labelBuf[0] = '\0';
			}

			ImGui::SameLine();
			size_t snapCount = 0;
			{
				const std::lock_guard<std::mutex> lock(g_SnapshotsMutex);
				snapCount = g_Snapshots.size();
			}
			ImGui::Text("(%zu captured)", snapCount);

			if (snapCount > 0)
			{
				if (ImGui::Button("Copy all as JSON##snapshots"))
				{
					json bundle;
					bundle["schemaVersion"] = SNAPSHOT_SCHEMA_VERSION;
					bundle["exportedAtMs"] = NowMs();
					bundle["snapshots"] = json::array();
					{
						const std::lock_guard<std::mutex> lock(g_SnapshotsMutex);
						for (const StoredSnapshot& s : g_Snapshots)
						{
							bundle["snapshots"].push_back(json::parse(s.Json, nullptr, false));
						}
					}
					std::string out = bundle.dump(2);
					ImGui::SetClipboardText(out.c_str());
					Log("Snapshot", "copied bundle of %zu (%zu bytes) to clipboard", snapCount, out.size());
				}
				ImGui::SameLine();
				if (ImGui::Button("Save all to file##snapshots"))
				{
					json bundle;
					bundle["schemaVersion"] = SNAPSHOT_SCHEMA_VERSION;
					bundle["exportedAtMs"] = NowMs();
					bundle["snapshots"] = json::array();
					{
						const std::lock_guard<std::mutex> lock(g_SnapshotsMutex);
						for (const StoredSnapshot& s : g_Snapshots)
						{
							bundle["snapshots"].push_back(json::parse(s.Json, nullptr, false));
						}
					}
					std::filesystem::path path = SnapshotDir() / BuildFilename("snapshots");
					std::ofstream f(path);
					f << bundle.dump(2);
					g_LastExportPath = path.string();
					Log("Snapshot", "saved bundle to %s", g_LastExportPath.c_str());
				}
				ImGui::SameLine();
				if (ImGui::Button("Clear all##snapshots"))
				{
					const std::lock_guard<std::mutex> lock(g_SnapshotsMutex);
					g_Snapshots.clear();
				}
			}

			if (!g_LastExportPath.empty())
			{
				ImGui::TextDisabled("Last saved to: %s", g_LastExportPath.c_str());
			}

			if (snapCount > 0)
			{
				if (ImGui::BeginTable("##snaptable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
				{
					ImGui::TableSetupColumn("#");
					ImGui::TableSetupColumn("Time");
					ImGui::TableSetupColumn("Label / inferred state");
					ImGui::TableSetupColumn("");
					ImGui::TableHeadersRow();

					std::vector<StoredSnapshot> snap;
					{
						const std::lock_guard<std::mutex> lock(g_SnapshotsMutex);
						snap = g_Snapshots;
					}

					int removeIdx = -1;
					for (size_t i = 0; i < snap.size(); i++)
					{
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::Text("%zu", i + 1);
						ImGui::TableSetColumnIndex(1);
						ImGui::TextUnformatted(FormatHms(snap[i].TimestampMs).c_str());
						ImGui::TableSetColumnIndex(2);
						/* re-parse inferred state out of the stored json */
						std::string inferred;
						{
							json j = json::parse(snap[i].Json, nullptr, false);
							if (j.is_object() && j.contains("inferredState"))
							{
								inferred = j["inferredState"].get<std::string>();
							}
						}
						ImGui::TextWrapped("%s  %s%s",
							snap[i].Label.c_str(),
							inferred.empty() ? "" : "— ",
							inferred.c_str());
						ImGui::TableSetColumnIndex(3);
						std::string rmLabel = "remove##snap" + std::to_string(i);
						if (ImGui::SmallButton(rmLabel.c_str()))
						{
							removeIdx = (int)i;
						}
					}
					ImGui::EndTable();

					if (removeIdx >= 0)
					{
						const std::lock_guard<std::mutex> lock(g_SnapshotsMutex);
						if ((size_t)removeIdx < g_Snapshots.size())
						{
							g_Snapshots.erase(g_Snapshots.begin() + removeIdx);
						}
					}
				}
			}

			ImGui::Separator();

			/* ---------------- recording ---------------- */

			bool recOn = g_RecordingOn.load();
			if (ImGui::Checkbox("Record samples (20 Hz) while this box is checked", &recOn))
			{
				g_RecordingOn.store(recOn);
				Log("Recording", recOn ? "started" : "stopped");
			}

			size_t sampleCount = 0;
			{
				const std::lock_guard<std::mutex> lock(g_RecordingMutex);
				sampleCount = g_RecordingSamples.size();
			}
			ImGui::SameLine();
			ImGui::Text("%zu samples", sampleCount);

			ImGui::TextDisabled("Keeps the last %u samples (%.1f min @ 20 Hz). Rolling buffer.",
				(unsigned)RECORDING_CAPACITY,
				(RECORDING_CAPACITY * RECORDING_INTERVAL_MS) / 60000.0f);

			if (sampleCount > 0)
			{
				if (ImGui::Button("Copy recording as JSON"))
				{
					json bundle;
					bundle["schemaVersion"] = SNAPSHOT_SCHEMA_VERSION;
					bundle["kind"] = "recording";
					bundle["intervalMs"] = RECORDING_INTERVAL_MS;
					bundle["exportedAtMs"] = NowMs();
					bundle["samples"] = json::array();
					{
						const std::lock_guard<std::mutex> lock(g_RecordingMutex);
						for (const json& s : g_RecordingSamples)
						{
							bundle["samples"].push_back(s);
						}
					}
					std::string out = bundle.dump();
					ImGui::SetClipboardText(out.c_str());
					Log("Recording", "copied %zu samples (%zu bytes) to clipboard", sampleCount, out.size());
				}
				ImGui::SameLine();
				if (ImGui::Button("Save recording to file"))
				{
					json bundle;
					bundle["schemaVersion"] = SNAPSHOT_SCHEMA_VERSION;
					bundle["kind"] = "recording";
					bundle["intervalMs"] = RECORDING_INTERVAL_MS;
					bundle["exportedAtMs"] = NowMs();
					bundle["samples"] = json::array();
					{
						const std::lock_guard<std::mutex> lock(g_RecordingMutex);
						for (const json& s : g_RecordingSamples)
						{
							bundle["samples"].push_back(s);
						}
					}
					std::filesystem::path path = SnapshotDir() / BuildFilename("recording");
					std::ofstream f(path);
					f << bundle.dump();
					g_LastRecordingPath = path.string();
					Log("Recording", "saved %zu samples to %s", sampleCount, g_LastRecordingPath.c_str());
				}
				ImGui::SameLine();
				if (ImGui::Button("Clear recording"))
				{
					const std::lock_guard<std::mutex> lock(g_RecordingMutex);
					g_RecordingSamples.clear();
				}
			}

			if (!g_LastRecordingPath.empty())
			{
				ImGui::TextDisabled("Last saved to: %s", g_LastRecordingPath.c_str());
			}
		}

		ImGui::Separator();

		/* ---------------- cursor & AC ---------------- */

		if (ImGui::CollapsingHeader("Cursor & Action Camera", ImGuiTreeNodeFlags_DefaultOpen))
		{
			CURSORINFO ci{};
			ci.cbSize = sizeof(ci);
			BOOL ok = GetCursorInfo(&ci);

			ImGui::Text("GetCursorInfo:          %s", ok ? "ok" : "FAILED");
			ImGui::Text("flags:                  0x%08X", (unsigned)ci.flags);
			ImGui::Text("  CURSOR_SHOWING:       %s", (ci.flags & CURSOR_SHOWING) ? "yes" : "no");
			ImGui::Text("  CURSOR_SUPPRESSED:    %s", (ci.flags & CURSOR_SUPPRESSED) ? "yes" : "no");
			ImGui::Text("IsCursorHidden (Win32): %s", !(ci.flags & CURSOR_SHOWING) ? "YES" : "no");
			ImGui::Text("ptScreenPos:            (%ld, %ld)", ci.ptScreenPos.x, ci.ptScreenPos.y);

			POINT p{};
			GetCursorPos(&p);
			ImGui::Text("GetCursorPos:           (%ld, %ld)", p.x, p.y);

			ImVec2 imgui_pos = ImGui::GetMousePos();
			ImGui::Text("ImGui::GetMousePos:     (%.0f, %.0f)", imgui_pos.x, imgui_pos.y);

			RECT clipRect{};
			if (GetClipCursor(&clipRect))
			{
				LONG w = clipRect.right - clipRect.left;
				LONG h = clipRect.bottom - clipRect.top;
				ImGui::Text("Cursor clip rect:       (%ld,%ld)-(%ld,%ld)  %ldx%ld  %s",
					clipRect.left, clipRect.top, clipRect.right, clipRect.bottom, w, h,
					(w <= 2 && h <= 2) ? "[PINNED 1x1 - Nexus MouseResetFix is active]" : "");
			}

			ImGui::Separator();

			bool acBoundInNexus = APIDefs ? APIDefs->GameBinds.IsBound(EGameBinds_CameraActionMode) : false;
			if (!acBoundInNexus)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
				ImGui::Text("CameraActionMode BIND MISSING in Nexus Game Keybinds.");
				ImGui::PopStyleColor();
				ImGui::TextDisabled("The addon's auto-toggle will be a no-op until you set it.");
			}
			else
			{
				ImGui::Text("CameraActionMode bound:  yes");
			}

			if (RTAPIData)
			{
				ImGui::Text("RTAPI IsActionCamera:    %s", RTAPIData->IsActionCamera ? "YES" : "no");
				ImGui::TextDisabled("  (corroborates Win32; not used by addon logic)");
			}
			else
			{
				ImGui::TextDisabled("RTAPI not loaded — IsActionCamera bit unavailable.");
			}

			unsigned long long last_raw = g_LastRawInputTimeMs.load();
			ImGui::Text("Last WM_INPUT mouse:    %s  (%llu ms ago)",
				last_raw == 0 ? "never" : FormatHms(last_raw).c_str(),
				last_raw == 0 ? 0ull : (NowMs() - last_raw));
		}

		/* ---------------- raw input ---------------- */

		if (ImGui::CollapsingHeader("Raw Input (WM_INPUT)", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Registered:             %s", g_RegisteredHwnd ? "yes" : "no");
			ImGui::Text("Hwnd:                   %p", g_RegisteredHwnd);
			ImGui::Text("Total WM_INPUT mouse:   %llu", g_RawInputCount.load());
			ImGui::Text("Last delta:             (%ld, %ld)", g_LastRawDx.load(), g_LastRawDy.load());
			ImGui::Text("Accumulated (session):  (%lld, %lld)", g_RawAccumX.load(), g_RawAccumY.load());
			if (ImGui::SmallButton("Reset accumulator"))
			{
				g_RawAccumX.store(0);
				g_RawAccumY.store(0);
			}
			ImGui::TextDisabled("If this still increments while AC is on → virtual-cursor feature is feasible.");
		}

		/* ---------------- WndProc counters ---------------- */

		if (ImGui::CollapsingHeader("WndProc message counters"))
		{
			std::map<UINT, unsigned long long> snapshot;
			{
				const std::lock_guard<std::mutex> lock(g_WndMutex);
				snapshot = g_WndCounts;
			}
			if (ImGui::BeginTable("##wndcounts", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("Message");
				ImGui::TableSetupColumn("Count");
				ImGui::TableHeadersRow();
				for (const auto& [m, c] : snapshot)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::Text("%s", MsgName(m) ? MsgName(m) : "?");
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%llu", c);
				}
				ImGui::EndTable();
			}
		}

		/* ---------------- GameBinds probe ---------------- */

		if (ImGui::CollapsingHeader("GameBinds probe (Nexus)"))
		{
			if (!APIDefs)
			{
				ImGui::TextDisabled("APIDefs null.");
			}
			else if (ImGui::BeginTable("##gamebinds", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("GameBind");
				ImGui::TableSetupColumn("IsBound in Nexus?");
				ImGui::TableHeadersRow();
				for (const ProbeEntry& e : kProbeBinds)
				{
					bool bound = APIDefs->GameBinds.IsBound(e.Bind);
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::Text("%s", e.Label);
					ImGui::TableSetColumnIndex(1);
					if (bound)
					{
						ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "yes");
					}
					else
					{
						ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "NOT BOUND");
					}
				}
				ImGui::EndTable();
			}
		}

		/* ---------------- MumbleLink Data ---------------- */

		if (ImGui::CollapsingHeader("MumbleLink — Data"))
		{
			if (!MumbleLink)
			{
				ImGui::TextDisabled("MumbleLink null.");
			}
			else
			{
				const Mumble::Data& d = *MumbleLink;
				const Mumble::Context& c = d.Context;

				ImGui::Text("UIVersion:              %u", d.UIVersion);
				ImGui::Text("UITick:                 %u", d.UITick);
				ImGui::Separator();
				ImGui::Text("AvatarPosition:         (%.3f, %.3f, %.3f)", d.AvatarPosition.X, d.AvatarPosition.Y, d.AvatarPosition.Z);
				ImGui::Text("AvatarFront:            (%.3f, %.3f, %.3f)", d.AvatarFront.X, d.AvatarFront.Y, d.AvatarFront.Z);
				ImGui::Text("AvatarTop:              (%.3f, %.3f, %.3f)", d.AvatarTop.X, d.AvatarTop.Y, d.AvatarTop.Z);
				ImGui::Separator();
				ImGui::Text("CameraPosition:         (%.3f, %.3f, %.3f)", d.CameraPosition.X, d.CameraPosition.Y, d.CameraPosition.Z);
				ImGui::Text("CameraFront:            (%.3f, %.3f, %.3f)", d.CameraFront.X, d.CameraFront.Y, d.CameraFront.Z);
				ImGui::Text("CameraTop:              (%.3f, %.3f, %.3f)", d.CameraTop.X, d.CameraTop.Y, d.CameraTop.Z);
				ImGui::Separator();
				ImGui::Text("MapID:                  %u", c.MapID);
				ImGui::Text("MapType:                %s", MapTypeName(c.MapType));
				ImGui::Text("ShardID:                %u", c.ShardID);
				ImGui::Text("InstanceID:             %u", c.InstanceID);
				ImGui::Text("BuildID:                %u", c.BuildID);
				ImGui::Text("MountIndex:             %s (%d)", MountIndexName(c.MountIndex), (int)c.MountIndex);
				ImGui::Text("ProcessID:              %u", c.ProcessID);
				ImGui::Separator();
				ImGui::Text("IsMapOpen:              %u", (unsigned)c.IsMapOpen);
				ImGui::Text("IsCompassTopRight:      %u", (unsigned)c.IsCompassTopRight);
				ImGui::Text("IsCompassRotating:      %u", (unsigned)c.IsCompassRotating);
				ImGui::Text("IsGameFocused:          %u", (unsigned)c.IsGameFocused);
				ImGui::Text("IsCompetitive:          %u", (unsigned)c.IsCompetitive);
				ImGui::Text("IsTextboxFocused:       %u", (unsigned)c.IsTextboxFocused);
				ImGui::Text("IsInCombat:             %u", (unsigned)c.IsInCombat);
				ImGui::Separator();
				ImGui::Text("Compass: %ux%u  rot=%.3f  center=(%.1f,%.1f)  player=(%.1f,%.1f)  scale=%.3f",
					c.Compass.Width, c.Compass.Height, c.Compass.Rotation,
					c.Compass.Center.X, c.Compass.Center.Y,
					c.Compass.PlayerPosition.X, c.Compass.PlayerPosition.Y,
					c.Compass.Scale);
			}
		}

		/* ---------------- MumbleLink Identity ---------------- */

		if (ImGui::CollapsingHeader("MumbleLink — Identity"))
		{
			if (!MumbleIdentity)
			{
				ImGui::TextDisabled("MumbleIdentity null. Wait for EV_MUMBLE_IDENTITY_UPDATED.");
			}
			else
			{
				const Mumble::Identity& id = *MumbleIdentity;
				ImGui::Text("Name:                   %s", id.Name);
				ImGui::Text("Profession:             %s (%d)", ProfessionName(id.Profession), (int)id.Profession);
				ImGui::Text("Specialization:         %u", id.Specialization);
				ImGui::Text("Race:                   %s (%d)", RaceName(id.Race), (int)id.Race);
				ImGui::Text("MapID:                  %u", id.MapID);
				ImGui::Text("WorldID:                %u", id.WorldID);
				ImGui::Text("TeamColorID:            %u", id.TeamColorID);
				ImGui::Text("IsCommander:            %s", id.IsCommander ? "yes" : "no");
				ImGui::Text("FOV:                    %.3f", id.FOV);
				ImGui::Text("UISize:                 %d", (int)id.UISize);
			}
		}

		/* ---------------- NexusLink ---------------- */

		if (ImGui::CollapsingHeader("NexusLink"))
		{
			if (!NexusLink)
			{
				ImGui::TextDisabled("NexusLink null.");
			}
			else
			{
				ImGui::Text("Width:                  %u", NexusLink->Width);
				ImGui::Text("Height:                 %u", NexusLink->Height);
				ImGui::Text("Scaling:                %.3f", NexusLink->Scaling);
				ImGui::Text("IsMoving:               %s", NexusLink->IsMoving ? "yes" : "no");
				ImGui::Text("IsCameraMoving:         %s", NexusLink->IsCameraMoving ? "yes" : "no");
				ImGui::Text("IsGameplay:             %s", NexusLink->IsGameplay ? "yes" : "no");
			}
		}

		/* ---------------- RTAPI ---------------- */

		if (ImGui::CollapsingHeader("RTAPI — RealTimeData"))
		{
			if (!RTAPIData)
			{
				ImGui::TextDisabled("RTAPI not loaded (install the separate RTAPI addon to populate this).");
			}
			else
			{
				const RTAPI::RealTimeData& r = *RTAPIData;
				ImGui::Text("GameBuild:              %u", r.GameBuild);
				ImGui::Text("GameState:              %s", RtapiGameStateName(r.GameState));
				ImGui::Text("Language:               %s", RtapiLanguageName(r.Language));
				ImGui::Text("TimeOfDay:              %s", RtapiTimeOfDayName(r.TimeOfDay));
				ImGui::Separator();
				ImGui::Text("MapID:                  %u", r.MapID);
				ImGui::Text("MapType:                %u", (unsigned)r.MapType);
				ImGui::Text("IPAddress:              %u.%u.%u.%u", r.IPAddress[0], r.IPAddress[1], r.IPAddress[2], r.IPAddress[3]);
				ImGui::Text("Cursor (world):         (%.2f, %.2f, %.2f)", r.Cursor[0], r.Cursor[1], r.Cursor[2]);
				ImGui::Separator();
				ImGui::Text("GroupType:              %s", RtapiGroupTypeName(r.GroupType));
				ImGui::Text("GroupMemberCount:       %u", r.GroupMemberCount);
				ImGui::Separator();
				ImGui::Text("AccountName:            %s", r.AccountName);
				ImGui::Text("CharacterName:          %s", r.CharacterName);
				ImGui::Text("CharacterPosition:      (%.2f, %.2f, %.2f)", r.CharacterPosition[0], r.CharacterPosition[1], r.CharacterPosition[2]);
				ImGui::Text("CharacterFacing:        (%.3f, %.3f, %.3f)", r.CharacterFacing[0], r.CharacterFacing[1], r.CharacterFacing[2]);
				ImGui::Text("Profession:             %u", r.Profession);
				ImGui::Text("EliteSpecialization:    %u", r.EliteSpecialization);
				ImGui::Text("MountIndex:             %u", r.MountIndex);
				ImGui::Separator();
				unsigned cs = (unsigned)r.CharacterState;
				ImGui::Text("CharacterState:         0x%08X", cs);
				ImGui::Text("  IsAlive:              %s", (cs & (unsigned)RTAPI::ECharacterState::IsAlive) ? "yes" : "no");
				ImGui::Text("  IsDowned:             %s", (cs & (unsigned)RTAPI::ECharacterState::IsDowned) ? "yes" : "no");
				ImGui::Text("  IsInCombat:           %s", (cs & (unsigned)RTAPI::ECharacterState::IsInCombat) ? "yes" : "no");
				ImGui::Text("  IsSwimming (surface): %s", (cs & (unsigned)RTAPI::ECharacterState::IsSwimming) ? "yes" : "no");
				ImGui::Text("  IsUnderwater:         %s", (cs & (unsigned)RTAPI::ECharacterState::IsUnderwater) ? "yes" : "no");
				ImGui::Text("  IsGliding:            %s", (cs & (unsigned)RTAPI::ECharacterState::IsGliding) ? "yes" : "no");
				ImGui::Text("  IsFlying:             %s", (cs & (unsigned)RTAPI::ECharacterState::IsFlying) ? "yes" : "no");
				ImGui::Separator();
				ImGui::Text("CameraPosition:         (%.2f, %.2f, %.2f)", r.CameraPosition[0], r.CameraPosition[1], r.CameraPosition[2]);
				ImGui::Text("CameraFacing:           (%.3f, %.3f, %.3f)", r.CameraFacing[0], r.CameraFacing[1], r.CameraFacing[2]);
				ImGui::Text("CameraFOV:              %.3f", r.CameraFOV);
				ImGui::Text("IsActionCamera:         %s", r.IsActionCamera ? "YES" : "no");
			}
		}

		/* ---------------- event log ---------------- */

		if (ImGui::CollapsingHeader("Event log", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextDisabled("last %u events (ring buffer)", (unsigned)LOG_CAPACITY);
			if (ImGui::BeginChild("##eventlog", ImVec2(0, 220), true, ImGuiWindowFlags_HorizontalScrollbar))
			{
				std::vector<LogEntry> snapshot;
				{
					const std::lock_guard<std::mutex> lock(g_LogMutex);
					snapshot.assign(g_Log.begin(), g_Log.end());
				}
				for (const LogEntry& e : snapshot)
				{
					ImGui::TextUnformatted(("[" + FormatHms(e.TimestampMs) + "] [" + e.Category + "] " + e.Message).c_str());
				}
				/* auto-scroll to bottom unless user is scrolling */
				if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
				{
					ImGui::SetScrollHereY(1.0f);
				}
			}
			ImGui::EndChild();
		}
	}
}
