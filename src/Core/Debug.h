///----------------------------------------------------------------------------------------------------
/// Copyright (c) Raidcore.GG - All rights reserved.
///
/// Name         :  Debug.h
/// Description  :  Live diagnostics panel: cursor/AC state, raw input deltas,
///                 Mumble/NexusLink/RTAPI dumps, rolling event log, GameBinds
///                 probe. Read-only; no behavior change when the panel is not
///                 open. Event logging and raw-input counters are always live.
/// Authors      :  Fork addition
///----------------------------------------------------------------------------------------------------

#ifndef DEBUG_H
#define DEBUG_H

#include <Windows.h>
#include <string>

namespace Debug
{
	///----------------------------------------------------------------------------------------------------
	/// Init:
	/// 	Registers for WM_INPUT raw mouse deltas on the given HWND.
	/// 	Safe to call multiple times; subsequent calls are no-ops.
	/// 	Called the first time Addon::WndProc sees a non-null HWND.
	///----------------------------------------------------------------------------------------------------
	void Init(HWND aGameWindow);

	///----------------------------------------------------------------------------------------------------
	/// Shutdown:
	/// 	Drops raw-input registration. Called from Addon::Unload.
	///----------------------------------------------------------------------------------------------------
	void Shutdown();

	///----------------------------------------------------------------------------------------------------
	/// OnWndProc:
	/// 	Handles WM_INPUT (delta accumulation) and counts selected messages.
	/// 	Returns uMsg so the caller always forwards. Called from Addon::WndProc.
	///----------------------------------------------------------------------------------------------------
	UINT OnWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

	///----------------------------------------------------------------------------------------------------
	/// Log:
	/// 	Appends a printf-style entry to the rolling event log.
	/// 	aCategory is a short tag (<= 16 chars recommended) shown as a prefix.
	///----------------------------------------------------------------------------------------------------
	void Log(const char* aCategory, const char* aFormat, ...);

	///----------------------------------------------------------------------------------------------------
	/// Specific event helpers for call-site clarity and consistency.
	///----------------------------------------------------------------------------------------------------
	void OnRadialActivateEvaluated(const std::string& aRadialName, bool aWasActionCamActive, bool aLeftHeld, bool aRightHeld);
	void OnActionCamToggleIssued(bool aDirection /*true = opening*/);
	void OnCursorWarpIssued(int aX, int aY, const char* aReason);
	void OnRadialReleased(const std::string& aRadialName, int aSelectionMode, int aHoverIndex);

	///----------------------------------------------------------------------------------------------------
	/// RenderPanel:
	/// 	Renders the full debug panel. Expected to be called inside an active
	/// 	ImGui window/tab. Uses CollapsingHeaders so sections can be toggled.
	///----------------------------------------------------------------------------------------------------
	void RenderPanel();
}

#endif
