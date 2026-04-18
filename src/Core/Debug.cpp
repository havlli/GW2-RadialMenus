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
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "imgui/imgui.h"

#include "RTAPI/RTAPI.hpp"

#include "Shared.h"
#include "Util.h"

namespace
{
	constexpr size_t LOG_CAPACITY = 256;

	struct LogEntry
	{
		unsigned long long TimestampMs; // since Init
		std::string        Category;
		std::string        Message;
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
		if (ImGui::SmallButton("Clear log"))
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
