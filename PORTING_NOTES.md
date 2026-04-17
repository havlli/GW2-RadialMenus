# PORTING_NOTES.md

Phase 0 research for porting selected UX features from [Friendly0Fire/GW2Radial](https://github.com/Friendly0Fire/GW2Radial) (abandoned, MIT) into a fork of [RaidcoreGG/GW2-RadialMenus](https://github.com/RaidcoreGG/GW2-RadialMenus) (active, MIT). This document is the source of truth for the project; update it as understanding changes.

Both projects are MIT. Lifted code must carry Friendly0Fire's copyright notice; existing Raidcore headers stay untouched.

---

## 1. GW2-RadialMenus anatomy

Rooted at `src/`. All paths relative to the repo root.

### 1.1 Entry / glue
- [src/entry.cpp](src/entry.cpp) — `DllMain` and the exported `GetAddonDef()`. Hands Nexus `Addon::Load` / `Addon::Unload`. Signature `67000002`, API version `NEXUS_API_VERSION` (currently 6).
- [src/Shared.h](src/Shared.h) / [src/Shared.cpp](src/Shared.cpp) — Globals shared across translation units: `APIDefs`, `SelfModule`, `WindowHandle`, `MumbleLink`, `MumbleIdentity`, `NexusLink`, `RTAPIData`, `RadialCtx`, plus the canonical directory paths (`AddonDirectory`, `PacksDirectory`, `IconsDirectory`).
- [src/Util.cpp](src/Util.cpp) / [src/Util.h](src/Util.h) — `String::Format/Replace`, `Time::GetTimestampMillis`, `URL::GetBase/GetEndpoint`. Also the `Resources::Unpack` helper for writing default PNG icons from embedded resources.
- [src/imgui_extensions.cpp](src/imgui_extensions.cpp) / [src/imgui_extensions.h](src/imgui_extensions.h) — Local ImGui extensions (`Angle`, `Distance`, `ImageRotated`, `HelpMarker`, `TooltipGeneric`, `CrossButton`, `ArrowButtonCondDisabled`, `ColorEdit4U32`, `TextURL`, `Animate`). `Animate` is the existing alpha/size lerp — reuse it for the fade-in feature.
- [src/GW2-RadialMenus.rc](src/GW2-RadialMenus.rc) / [src/resource.h](src/resource.h) — Embeds PNGs from [src/Resources/](src/Resources/) as WinAPI resources. Add new PNGs here.
- [src/Language.h](src/Language.h) — `Lang::Init(APIDefs->Localization.Set)` pre-populates `((SomeKey))` style strings. Every user-visible string should flow through this.

### 1.2 Core
- [src/Core/Addon.cpp](src/Core/Addon.cpp) + [src/Core/Addon.h](src/Core/Addon.h) — Thin layer that registers Nexus callbacks (`Render`, `RenderOptions`, `WndProc`) and wires events (`EV_MUMBLE_IDENTITY_UPDATED`, `EV_ADDON_LOADED`, `EV_ADDON_UNLOADED`). `Load()` at [Addon.cpp:30](src/Core/Addon.cpp#L30) is where RTAPI detection, first-install bootstrapping, and `RadialCtx` construction happen.
- [src/Core/RadialContext.h](src/Core/RadialContext.h) / [src/Core/RadialContext.cpp](src/Core/RadialContext.cpp) (2321 lines) — The singleton. Owns `Radials`, the `RadialIBMap` (keybind id → menu), the `ItemProcessor`, plus editor state. Key surfaces:
  - Lifecycle: `Load` / `Save` / `LoadInternal` / `SaveInternal` ([RadialContext.cpp:1989](src/Core/RadialContext.cpp#L1989)).
  - Activation: `Activate(CRadialMenu*)` / `Release(ESelectionMode)` ([RadialContext.cpp:625](src/Core/RadialContext.cpp#L625)).
  - Frame: `Render` ([RadialContext.cpp:689](src/Core/RadialContext.cpp#L689)) draws every registered menu and the item-processor widget.
  - UI: `RenderEditorTab` + `RenderSettingsTab` ([RadialContext.cpp:704](src/Core/RadialContext.cpp#L704)) — two ImGui tabs plus a debug tab.
  - Input: `OnInputBind` routes Nexus keybind callbacks via `RadialIBMap`; `WndProc` translates `WM_LBUTTONDOWN` / `WM_KEYDOWN VK_ESCAPE` / etc. into `Release(ESelectionMode::*)` calls.
  - Bootstrap: `CreateDefaultMountRadial` ([RadialContext.cpp:567](src/Core/RadialContext.cpp#L567)) writes the shipped mount radial on first install.
- [src/Core/RadialMenu.h](src/Core/RadialMenu.h) / [src/Core/RadialMenu.cpp](src/Core/RadialMenu.cpp) (1194 lines) — Per-menu state and renderer. Public fields include `DrawInCenter`, `DoNotCenterCursor`, `RestoreCursor`, `Scale`, `IconScale`, `HoverTimeout`, `ItemRotationDegrees`, `ShowItemNameTooltip`, `SpecificCenterItemName`. Draw loop is at [RadialMenu.cpp:173](src/Core/RadialMenu.cpp#L173); activation at [RadialMenu.cpp:367](src/Core/RadialMenu.cpp#L367); release at [RadialMenu.cpp:458](src/Core/RadialMenu.cpp#L458); placement math (`cos/sin` around `Origin`) at [RadialMenu.cpp:317-321](src/Core/RadialMenu.cpp#L317-L321); hit-testing at [RadialMenu.cpp:1008](src/Core/RadialMenu.cpp#L1008). Persistence in `Save()` at [RadialMenu.cpp:83](src/Core/RadialMenu.cpp#L83) — **this is the canonical on-disk schema**.
- [src/Core/ItemProcessor.h](src/Core/ItemProcessor.h) / [src/Core/ItemProcessor.cpp](src/Core/ItemProcessor.cpp) — Background thread (`Process`) that dequeues one `RadialItem*` at a time, awaits its Activation `Conditions`, executes its action list (`InputBind`, `GameInputBind[Press|Release]`, `Event`, `Delay`, `Return`). Renders the in-world "queued action" widget ring, `AwaitActivation` at [ItemProcessor.cpp:259](src/Core/ItemProcessor.cpp#L259). **This is the thing we extend for Feature 4 (combat queue)** — the plumbing already exists, we just need to bias timeouts/cancellations.
- [src/Core/StateObserver.h](src/Core/StateObserver.h) / [src/Core/StateObserver.cpp](src/Core/StateObserver.cpp) — Each frame, `Advance()` derives a `Conditions` struct from MumbleLink + (optionally) RTAPI. Handles `IsCombat`, `IsMounted` (with enum `EObserveMount`), `IsCommander`, `IsCompetitive`, `IsMapOpen`, `IsTextboxActive`, `IsInstance`, `IsGameplay`, `IsUnderwater`, `IsOnWaterSurface`, `IsAirborne`. **This is the only place that reads game state; all conditional features route through `IsMatch`.**
- [src/Core/Conditions.h](src/Core/Conditions.h) / [src/Core/Conditions.cpp](src/Core/Conditions.cpp) — POD for serializable state expectations plus `to_json`/`from_json`.
- [src/Core/Action.h](src/Core/Action.h) — `ActionBase` / `ActionGeneric` / `ActionGameInputBind` / `ActionDelay` hierarchy. All actions share `Type`, `Activation`, `OnlyExecuteIfPrevious`.
- [src/Core/RadialItem.h](src/Core/RadialItem.h) — POD: `Identifier`, `Priority`, `Color` / `ColorHover`, `Icon`, `Visibility`, `Activation`, `ActivationTimeout`, `Actions`, `DisplayItemSize`.
- [src/Core/Icon.h](src/Core/Icon.h), [src/Core/EIconType.h](src/Core/EIconType.h), [src/Core/ERadialType.h](src/Core/ERadialType.h), [src/Core/EInnerRadius.h](src/Core/EInnerRadius.h), [src/Core/ESelectionMode.h](src/Core/ESelectionMode.h), [src/Core/ECenterBehavior.h](src/Core/ECenterBehavior.h), [src/Core/EActionType.h](src/Core/EActionType.h) — enums, self-explanatory.

### 1.3 Thirdparty (submodules)
- [src/thirdparty/Nexus](src/thirdparty/Nexus) — `RaidcoreGG/RCGG-lib-nexus-api`, pinned to a v6 commit.
- [src/thirdparty/Mumble](src/thirdparty/Mumble) — shared struct for the Mumble Link.
- [src/thirdparty/RTAPI](src/thirdparty/RTAPI) — `RealTimeData` struct (RTAPI addon must be installed by the user).
- [src/thirdparty/ImAnimate](src/thirdparty/ImAnimate) — in-house ImGui tween helper; already used via `ImGui::Animate`.
- [src/thirdparty/imgui](src/thirdparty/imgui) — RaidcoreGG fork of Dear ImGui. Includes `ImGui::ImageRotated` and a few other extensions.
- [src/thirdparty/nlohmann](src/thirdparty/nlohmann) — header-only JSON. Already present; do not re-vendor.

### 1.4 Runtime file layout
- `<GW2>/addons/radials.dll` — the built DLL (output name per `.vcxproj`).
- `<GW2>/addons/RadialMenus/packs/*.json` — one JSON per radial menu. Schema is the `CRadialMenu::Save()` output (see [RadialMenu.cpp:83](src/Core/RadialMenu.cpp#L83)). `FormatRevision: 2` is current; `1` still parses with the `IsMounted` enum remap at [RadialContext.cpp:2083](src/Core/RadialContext.cpp#L2083).
- `<GW2>/addons/RadialMenus/icons/` — user icons. Default mount icons are dropped here on first install.

### 1.5 Nexus entry points (summary)
- `Addon::Load(aApi)` at [Addon.cpp:30](src/Core/Addon.cpp#L30) — pulls DataLinks, registers render callbacks for `ERenderType_Render` / `ERenderType_OptionsRender`, and `WndProc`.
- Keybind → wheel: `OnInputBind` → `RadialCtx->OnInputBind` → `RadialContext::Activate` → `CRadialMenu::Activate` (at [RadialMenu.cpp:367](src/Core/RadialMenu.cpp#L367)).
- Wheel → selection: `WndProc` catches `WM_LBUTTONDOWN` / `WM_ESCAPE` → `RadialContext::Release` → `CRadialMenu::Release` → `RadialCtx->QueueItem` → `CItemProcessor::QueueItem`.
- Config load/save: `CRadialContext::Load()` at [RadialContext.cpp:613](src/Core/RadialContext.cpp#L613) walks `PacksDirectory`, parses each `.json`, and rebuilds menus. Each menu re-serializes itself in `CRadialMenu::Save()` ([RadialMenu.cpp:83](src/Core/RadialMenu.cpp#L83)).

---

## 2. Nexus public API surface we actually touch

Full header: [src/thirdparty/Nexus/Nexus.h](src/thirdparty/Nexus/Nexus.h). `NEXUS_API_VERSION = 6` currently.

### 2.1 Subsystems
- `APIDefs->Renderer.Register(ERenderType_Render | ERenderType_OptionsRender, GUI_RENDER)` — hook points for draw. `OptionsRender` is the "Nexus ⇒ Options ⇒ Addons ⇒ Radial Menus" tab; `Render` is every frame.
- `APIDefs->WndProc.{Register,Deregister,SendToGameOnly}` — raw Win32 message pipeline. Return `0` from the callback to swallow the message; `uMsg` to pass through.
- `APIDefs->InputBinds.{RegisterWithString,RegisterWithStruct,Invoke,Deregister}` — Nexus-side keybinds. We register `KB_RADIAL<ID>` strings.
- `APIDefs->GameBinds.{Press,Release,InvokeAsync,PressAsync,ReleaseAsync,IsBound}` — the EGameBinds (game-side) press/release. `Press`/`Release` are sync, `*Async` schedule a thread. The enum is the game's internal action id; see [Nexus.h:114](src/thirdparty/Nexus/Nexus.h#L114).
- `APIDefs->Events.{Subscribe,Unsubscribe,Raise,RaiseNotification,RaiseTargeted}` — pub/sub. Payloads are opaque `void*`.
- `APIDefs->DataLink.Get(identifier)` — shared memory regions. We use `DL_MUMBLE_LINK`, `DL_NEXUS_LINK`, `DL_MUMBLE_LINK_IDENTITY`, `DL_RTAPI`.
- `APIDefs->Textures.{Get,GetOrCreateFromResource,GetOrCreateFromFile,GetOrCreateFromURL,LoadFromFile,LoadFromURL}` — cached texture uploads; `Texture*` gives you `ID3D11ShaderResourceView*` to hand to ImGui.
- `APIDefs->UI.SendAlert` — transient top-of-screen toast. Use sparingly.
- `APIDefs->Log(ELogLevel, channel, msg)` — **the only correct log path**. No `printf`, `OutputDebugString`, `std::cout`.
- `APIDefs->Localization.{Translate,TranslateTo,Set}` — i18n. Register English defaults in `Lang::Init`.
- `APIDefs->Paths.{GetGameDirectory,GetAddonDirectory,GetCommonDirectory}` — canonical directories.

### 2.2 DataLink structures of interest
- `NexusLinkData` ([Nexus.h:381](src/thirdparty/Nexus/Nexus.h#L381)) — `Width`, `Height`, `Scaling`, `IsMoving`, `IsCameraMoving`, `IsGameplay`, plus ImFont pointers. Render every frame using `NexusLink->Scaling` for DPI.
- `Mumble::Data` / `Mumble::Identity` ([Mumble.h](src/thirdparty/Mumble/Mumble.h)) — avatar position, camera, map id, map type (inc. `WvW_*`), mount index, is-in-combat bit. **No `IsUnderwater`, no `IsGliding`** — those are inferred.
- `RTAPI::RealTimeData` ([RTAPI.hpp](src/thirdparty/RTAPI/RTAPI.hpp)) — adds `IsActionCamera : 1`, `CharacterState` bitmask (`IsUnderwater`, `IsSwimming`, `IsGliding`, `IsFlying`), `GameState`, authoritative `MapType`, group data. Only populated if the user installed the separate RTAPI addon — check `RTAPIData && RTAPIData->GameBuild != 0` before dereferencing.

### 2.3 Known API quirks (from issue audit)
(Issue numbers are in RaidcoreGG/Nexus unless prefixed.)

- **#91 is the pending v7 rewrite.** v7 will: add sync `GameBinds.Invoke()`, add `GameBinds.Get`, add `GameBinds.SendChatMessage`, expose `InputBinds.{Get,Press,Release,IsBound,Delete}`, **drop `hWnd` from `WndProc.SendToGameOnly`**, add versioned DataLink identifiers, merge Remote+Endpoint into a single URL. **Implication:** wrap every Nexus call we use behind a thin internal facade so the eventual v7 port is a one-file change.
- **GameBinds.Invoke is async-only in v6** (#91 thread). Do not read state synchronously after `Press` / `InvokeAsync`; the game has not yet processed it.
- **GameBind enum can lag game updates** (#70, #98). Legacy bind ids get repurposed and addons must migrate. We already have one such remap at [RadialContext.cpp:2126](src/Core/RadialContext.cpp#L2126) (`(EGameBinds)10 → EGameBinds_MoveJump_SwimUp_FlyUp`). Treat the enum as best-effort; always null-check `APIDefs->GameBinds.IsBound(bind)` before relying on it.
- **Events are raised on detached `std::thread` per-raise** (#111 open). Ordering is not guaranteed. **Do not call `Events.Raise` from inside an event callback on older Nexus — recursive mutex bug (#109) deadlocked prior to 2025-04-23.**
- **DataLink may be null at `Load`** (#88). `MumbleIdentity` in particular comes in asynchronously — see the existing `OnMumbleIdentityUpdated` subscription in [Addon.cpp:22](src/Core/Addon.cpp#L22). Same applies to `RTAPIData` until the RTAPI addon loads (`EV_ADDON_LOADED` with `RTAPI_SIG`).
- **Textures from URL get re-downloaded if called per-frame** (#113 fixed 2025-04-25). Always cache the `Texture*` in the calling class; don't ask Nexus for it each frame.
- **Texture lifetime crash at shutdown with ReShade/DXVK** (#138 fixed 2025-11-10). Nothing we can do addon-side; users need current Nexus.
- **Linux (Proton) is a second-class citizen.** Keyboard modifier handling, alt-tab, focus-follows-mouse are all flakier. Bugs should assume Windows first.
- **`NEXUS_API_VERSION == 6` is what [src/thirdparty/Nexus/Nexus.h:10](src/thirdparty/Nexus/Nexus.h#L10) currently says.** v7 exists only in the issue tracker, not in code.

---

## 3. Feature → GW2Radial source map

GW2Radial lives at `https://github.com/Friendly0Fire/GW2Radial` (branch `master`). Raw base: `https://raw.githubusercontent.com/Friendly0Fire/GW2Radial/master/`. Layout: `include/*.h`, `src/*.cpp`. **Ignore `shaders/`, all MinHook hooks, all D3D device setup** — the renderer is replaced with ImGui.

License: MIT. Any file where we lift substantial logic must carry Friendly0Fire's copyright notice at the top.

Note on angular math: the per-slice `cos/sin` placement in GW2Radial lives in `shaders/WheelElement.hlsl` (GPU side). The C++ side only passes `elementCount` / `centerScale` to a constant buffer. Our port re-implements placement in C++ using the same polar formula — the existing `RadialMenu::Render` at [RadialMenu.cpp:317-321](src/Core/RadialMenu.cpp#L317-L321) already does this; we extend it.

### Feature 1 — Icon size + angular spread
- `include/Wheel.h:237-243` — option members: `scaleOption_` (default 1.0, slider `[0.25, 4.0]`), `centerScaleOption_` (0.2, `[0.05, 0.5]`), `opacityMultiplierOption_` (100, `[0, 100]`), `animationTimeOption_` (750 ms, `[0, 2000]`), `animationScale_` (1.0, `[0, 1]`).
- `src/Wheel.cpp:41-63` — option ctor defaults.
- `src/Wheel.cpp:201-214` — UI sliders.
- `src/Wheel.cpp:576-580` — `baseSpriteDimensions = scale * 0.5` in normalized space, aspect-corrected.
- `src/Wheel.cpp:122-153` (`UpdateHover`) — sole angular hit-test math in C++: `mouseAngle = atan2(-y, -x) - π/2`, `elementAngle = 2π / N`, `elementId = int((mouseAngle - elementAngle/2) / elementAngle + 1) % N`. Aspect correction multiplies `mousePos.y` by `screenH/screenW`. Central deadzone radius: `scale * 0.125 * 0.8 * centerScale`.
- Our equivalent: [RadialMenu.cpp:317-321](src/Core/RadialMenu.cpp#L317-L321) and [RadialMenu.cpp:1008-1036](src/Core/RadialMenu.cpp#L1008-L1036).
- **Port shape:** we already have `IconScale` ([RadialMenu.h:37](src/Core/RadialMenu.h#L37)) and a scale slider. Adding an "Angular spread" (multiplier on `SegmentRadius`) and explicit icon-size slider is a ~30-line change.
- **Related issues:** GW2Radial#287 "Jiggly wheel" (motion sickness, hover-wobble animation with no off-switch) — cautionary tale: every new animation needs an opt-out. GW2Radial#73, #63 (animation time slider).

### Feature 2 — Cursor-warp-to-center reticule (Draw in Center)
- `include/Wheel.h:213` — `centralKeybind_` (separate "Show in Center" bind).
- `src/Wheel.cpp:537-550` — `resetCursorPositionToCenter` lambda: `GetWindowRect` on the game HWND + `SetCursorPos` to `(screenW*0.5, screenH*0.5)`, then manually updates `ImGui::GetIO().MousePos`.
- `src/Wheel.cpp:991-1033` (`ActivateWheel`) — on activate with `isMountOverlayLocked`: `currentPosition_ = (0.5, 0.5)`, `resetCursorPositionToCenter_ = true`, deferred to next draw so it lands **after** `displayDelayOption_`.
- `src/Wheel.cpp:941-989` (`KeybindEvent`) — `center` param propagates from the bind callback into `ActivateWheel(center)`.
- `src/Wheel.cpp:1093` — on deactivate, `cursorResetPosition_` captured at activation is used to warp back (gated by `resetCursorAfterKeybindOption_`).
- **Upstream has already shipped the toggle.** Commit `6ddd529` ("Add option to not force cursor to center when drawing in center") introduced `DoNotCenterCursor` at [RadialMenu.h:34](src/Core/RadialMenu.h#L34) and the checkbox at [RadialContext.cpp:1218](src/Core/RadialContext.cpp#L1218). The remaining work from the kickoff prompt is the **in-menu reticule** drawing (a simple `ImDrawList::AddCircle` at `Origin`) and any tightening of the SetCursor timing relative to fade-in.
- **Related issues:**
  - GW2-RadialMenus#7 (closed) — user's original complaint; the `DoNotCenterCursor` toggle was the fix.
  - GW2Radial#269 / #247 (closed) — disabling "move cursor to original location" crashed the game until a late fix. **We must robust this path, not trust users' first `Release` invariants.**
  - GW2Radial#267 (closed) — quick-tap-before-fade selects wrong mount. **Interacts with Feature 6.** Solution was a "Behavior when released before delay has lapsed" option.
  - GW2Radial#236 (closed) + #344 (open) — Marker wheels require cursor to be warped back at release. Our Marker-style use cases (if any) need same.
  - GW2Radial#294 (closed) — High-DPI scaling applied to overlay but not inputs. Since we use `NexusLink->Scaling` everywhere, we should be fine, but worth a sanity check.

### Feature 3 — Dismount-on-same-mount toggle
- `src/MountWheel.cpp:114-144` (`MountWheel::BypassCheck`) — if `quickDismountOption_` is on and `MumbleLink::i().isMounted()`, set `we = previousUsed_` (or first usable) and **critically** set `kb = &dismountKeybind_` so the wheel skips opening and sends the in-game dismount bind directly (`Input::i().SendKeybind(bypassKeybind->keyCombo(), std::nullopt)`).
- `src/MountWheel.cpp:146-157` (`CustomDelayCheck`) — optional delay (`dismountDelayOption_`, 0–3000 ms) so the wheel swallows the input and re-fires.
- `include/MountWheel.h:122-129` — option + `dismountKeybind_`.
- **Crucially: GW2Radial does NOT compare `selected == current`.** It dismounts *any* mount when the bind is pressed while mounted. The kickoff prompt specifies a **stricter** behavior: if the player is already on the selected mount, send dismount; else send the mount key. That needs `MumbleLink->Context.MountIndex` compared to the item's mount. Our advantage: we're data-driven, so we can wire the comparison via an item's `Activation` conditions or a new per-item `DismountOnMatch` flag.
- **Related issues:**
  - GW2Radial#233 (closed) — the algorithm history (first active → last used; alt-account breakage).
  - GW2Radial#338 (closed) — without an explicit Dismount bind set in both the game AND the addon, queuing fires instead. **Our action schema must make this obvious** (likely a dedicated "Dismount" action type or a well-documented recipe).
  - GW2Radial#369 (open) — v2.4 regression: center=Favorite no longer dismounts in combat.
  - GW2Radial#373 (open) — chairs/zip-lines share the Dismount action; Radial queues a mount when dismounting from a chair. Edge case.
  - GW2Radial#243 (closed no-fix) — per-key auto-dismount flakiness with certain bind combos (backslash).
  - GW2-RadialMenus#18 (open, 2026-02-18) — direct feature request by an existing user: "Execute a specific item 'Mount/Dismount'." This is our upstream-PR candidate.

### Feature 4 — Combat input queuing
- `src/Wheel.cpp:217-228` (`ConditionalDelay` struct) + `230` member.
- `src/Wheel.cpp:1101-1129` (`SendKeybindOrDelay`) — if `enableQueuingOption_ && !element->isUsable(currentState)`, store element in `conditionalDelay_` with `time = now`. Sends a no-op input immediately so the key release is clean; real bind send deferred.
- `src/Wheel.cpp:451-513` (`OnUpdate`) — per-frame state machine: if `cd.element` set and `now > cd.time + maxWaitTime*1000` → give up. Else when `CanActivate(element)` passes continuously for `conditionalDelayDelayOption_` (default 200 ms) → fire via `Input::SendKeybind`. Lapsing condition resets `testPasses`.
- `src/Wheel.cpp:785` (`CanActivate`) — wraps `isUsable(currentState)`. `ConditionalState` is a bitmask over `MumbleLink::currentState()`.
- `include/Wheel.h:252-257` — options: `maximumConditionalWaitTimeOption_` (30 s default), `conditionalDelayDelayOption_` (200 ms debounce), `showDelayTimerOption_`, `centerCancelDelayedInputOption_`, `enableQueuingOption_`.
- Cancellation paths: map change (Wheel.cpp:515), focus lost (776), character change (520), Cancel slice (MountWheel.cpp:25-28), center-region click (Wheel.cpp:1048-1052), mounted externally (MountWheel.cpp:46-53). **Movement does NOT auto-cancel in GW2Radial** — that was a kickoff-prompt addition, not a port.
- **We already have the plumbing.** [ItemProcessor::AwaitActivation](src/Core/ItemProcessor.cpp#L259) polls `StateObserver::IsMatch(&Activation)` every 50 ms with a timeout. Adding a combat-specific queue is mostly:
  1. A new per-menu `QueueOnCombat` flag + per-item timeout override.
  2. A new cancel path in `ItemProcessor::Process` that watches for movement (`WM_KEYDOWN` on `W/A/S/D/Space`) or any other bind trigger.
- **Related issues:**
  - GW2-RadialMenus#15 (closed) — user asked for "mount as soon as out of combat"; maintainer pointed at visibility conditions. Activation `IsCombat: False` already gates (item waits until out of combat) but doesn't *queue* with user feedback — this feature closes that gap.
  - GW2Radial#387 (open), #370 (open) — disabling queuing checkbox only hides it visually; the mount still queues. **Anti-pattern: make the toggle a real no-op.**
  - GW2Radial#386 (open), #384 (closed) — Force button visually enabled but no keypress for Warclaw/Skimmer.
  - GW2Radial#377 (closed) — ArcDPS d3d11.dll ordering broke queuing. We're Nexus-native so less exposed, but Nexus load order can still matter.
  - GW2Radial#256 (closed) — queued-icon flicker with multi-box (Mumble-Link name per instance). We read `MumbleLink` directly, same exposure.
  - GW2Radial#368 (open) — non-mount binds (fishing/chair) in a mount slot spuriously trigger queuing.
  - GW2Radial#152 (closed) — maintainer explicitly refused "swap mount in one click" as **out of bounds per ArenaNet macro policy**. Keep our queue user-driven, not chained.
  - GW2Radial#221 (closed) — maintainer: "post-combat queue does not violate any rule." Useful precedent if our feature is ever questioned.

### Feature 5 — Conditional menus / smart automount
- `include/WheelElement.h:10-33` — `enum class ConditionalProperties` (Visible/Usable × Default/Underwater/OnWater/InCombat/WvW).
- `include/WheelElement.h:35-67` — pure `IsUsable(ConditionalState, ConditionalProperties)` / `IsVisible(...)`: if state bit set AND not permitted in that state → false. `None` state requires `*Default`.
- `include/WheelElement.h:185-193` — `isUsable`/`isVisible` combine a `customBehavior_` override (used for Cancel/Force pseudo-elements) with prop check.
- `include/MountWheel.h:73-110` (`GetMountPropsFromType`) — per-mount defaults: Skimmer + SiegeTurtle get `UsableOnWater | UsableUnderwater`; Warclaw gets `UsableWvW`; Skiff `UsableOnWater`.
- `src/Wheel.cpp:86-110` (`GetSkipState` / `ShouldSkip`) — smart-auto resolver: if `GetUsableElements(skipState).size() == 1`, return that single element immediately without drawing the wheel. Underwater-Skimmer and WvW-Warclaw "just work" via this path.
- `src/Wheel.cpp:848-911` — filter iteration.
- **Our equivalent is already 80% built.** [StateObserver](src/Core/StateObserver.cpp) computes `IsUnderwater`, `IsOnWaterSurface`, `IsCompetitive` (true in WvW+PvP), `IsCombat`. `Conditions` per-item already gate visibility and activation. The missing piece is the **skip-if-exactly-one-visible** fast-path in `CRadialMenu::Activate` — currently we do this at [RadialMenu.cpp:383-387](src/Core/RadialMenu.cpp#L383-L387) for the 1-item case, but only after activation; porting `ShouldSkip` semantics requires moving it earlier.
- **Related issues:**
  - GW2Radial#380 (open) + #359 (closed) — **Janthir grottos**: water volumes above `Z=0` and air volumes below `Z=0` break MumbleLink's underwater detection. Maintainer: "unfortunately impossible, sorry." RTAPI's `IsUnderwater` character-state bit is the workaround and is already wired in [StateObserver.cpp:198](src/Core/StateObserver.cpp#L198). **Document this as a user-visible caveat — recommend RTAPI in the README.**
  - GW2Radial#375 (open) — crashes when swimming + selecting skimmer/skiff with numpad+modifier bind. Input-handling edge.
  - GW2Radial#270 (closed) — SmartWar broke dismount if you went to WvW without opening wheel in PvE first. **State-init bug**: don't require a prior frame to initialize map/competitive state.
  - GW2Radial#208 (open) — multi-boxing leaks WvW detection across instances. MumbleLink-name-per-instance is the fix.
  - GW2-RadialMenus#17 (open) — `IsAirborne` flickers, and when switched to RTAPI the Skimmer is mis-flagged as airborne. RTAPI is **not** a silver bullet; conditions need hysteresis.
  - GW2-RadialMenus#13 (closed) — officially: "use RTAPI for non-zero-Z water bodies." Our existing RTAPI override at [StateObserver.cpp:167](src/Core/StateObserver.cpp#L167) already handles this.
  - GW2Radial#260 / #259 — **maintainer cannot detect gliding/falling/jumping/unauthorized-area** from MumbleLink. The hacky delta-Y heuristic in [StateObserver.cpp:73](src/Core/StateObserver.cpp#L73) is the best we can do without RTAPI.

### Feature 6 — Fade-in / easing on open
- `include/Wheel.h:242` — `animationTimeOption_` default 750 ms.
- `src/Wheel.cpp:582` — `fadeTimer = min(1, (now - (triggerTime + displayDelay)) / (animTime * 0.5f))`. **Linear ramp over half the configured duration.** Value is then handed to the shader which applies the real `smoothstep`.
- `src/Wheel.cpp:715` — queued-timer indicator: `min(absDt * 2, 1)` — same linear ×2 ramp, over `maximumConditionalWaitTime`.
- `src/Wheel.cpp:700-706` — `SmoothStep(dt * 3)` is the ONLY easing curve in C++; used for the first 333 ms of the queued-delay indicator size/position lerp.
- `src/Wheel.cpp:744-754` — `tiltMatrix` "3D tilt" based on mouse offset; magnitude clamped to 0.2, multiplied by `0.4 * animationScale_`. **Skip this** — it's what caused GW2Radial#287 (motion sickness).
- **Port shape:** we already have `ImGui::Animate` ([imgui_extensions.cpp](src/imgui_extensions.cpp), used at [RadialMenu.cpp:190-198](src/Core/RadialMenu.cpp#L190-L198)) with `ImAnimate::ECurve::InCubic`/`OutCubic`. The existing fade is 100 ms `In-Cubic`; we can widen it to `[0, 500]` ms with an optional `smoothstep` curve and gate it behind a per-menu `FadeDurationMs` field. **Default OFF if we alter existing behavior**; otherwise keep default at 100 ms for continuity.
- **Related issues:**
  - GW2Radial#287 (closed) — "jiggly wheel", no disable toggle. **Hard requirement:** our fade-in needs an off-switch.
  - GW2Radial#267 (closed) — fast-tap-before-fade-completes selects wrong mount (cursor hadn't warped yet). **Feature 6 must coordinate with Feature 2:** cursor warp must happen BEFORE the first hoverable frame, or the hover index must be forced to `-1` until fade-in completes.

---

## 4. Cross-cutting quirks & design implications

### 4.1 EULA / ArenaNet macro policy
Explicit statements from maintainers, grouped:
- **Allowed / blessed**: wheel selects one bind per interaction; post-combat queue (GW2Radial#221).
- **Out of bounds**: single-key chains that change multiple game states, "one-click swap mount" (GW2Radial#152).
- **Auto-detection of Action Camera**: the kickoff prompt states this was declined in GW2Radial for EULA reasons. **This rationale is NOT in the GitHub issue tracker** — it lives in README / Discord. The linked ANet macro policy ([help.guildwars2.com](https://help.guildwars2.com/hc/en-us/articles/360013762153-Policy-Macros-and-Macro-Use)) is already referenced in the addon at [RadialContext.cpp:1427](src/Core/RadialContext.cpp#L1427). **Design rule: no hook on Action Camera, manual-toggle only.**
- Existing Action-Cam handling in our addon ([RadialMenu.cpp:415-426](src/Core/RadialMenu.cpp#L415-L426)) toggles `EGameBinds_CameraActionMode` via `GameBinds.Press/Release` — this is user-initiated (they pressed the radial bind) and legal.

### 4.2 Action-Camera-specific issues
- GW2-RadialMenus#4 (open since 2024-10) — "action-ifying the input method." Maintainer committed to direction-as-input but hasn't shipped. **This is the single biggest unmet UX gap upstream.** Out of scope for our six features but worth tracking.
- GW2-RadialMenus#19 (open 2026-03) — fresh request to preserve camera control while menu up.
- GW2-RadialMenus#3 (closed) — users must bind `CameraActionMode` in Nexus Game Keybinds for the addon to disable/re-enable it.

### 4.3 State detection is fundamentally lossy
- MumbleLink does not expose: underwater (post-SotO water volumes), gliding, falling, jumping, dismounting, chat focus, trading-post focus, unauthorized-mount areas.
- RTAPI helps for underwater/swimming/gliding/flying but introduces its own false positives (Skimmer mis-flagged airborne — GW2-RadialMenus#17, commit `c495ad3` "Ugly hack for RTAPI falling/ascending").
- Our heuristic deltaY approach in [StateObserver.cpp:73](src/Core/StateObserver.cpp#L73) is best-effort and will misfire.
- **Design rule:** state-dependent features must gracefully no-op on ambiguity. Never lock the user out of a mount because state detection was wrong. Prefer "queue with visible timeout" over "block."
- **Document RTAPI as "strongly recommended"** in the README for anyone using underwater/WvW/combat conditions.

### 4.4 Input & modifiers
- GW2Radial#199, #12, #278, #262 — Alt/Shift modifier fall-through and stuck-key-after-alt-tab is a recurring pain. Nexus's `InputBinds.RegisterWithString` has had its own history with this (Nexus#72, #74).
- GW2-RadialMenus#2 (open) — modifier fall-through is reproduced here too, and radial-calling-radial has crashed. **Reentrancy guard needed** anywhere new features chain binds.
- GW2Radial#335, #263 — chat / trading-post focus is undetectable, so wheels open while typing. We already expose `IsTextboxActive` via MumbleLink's `IsTextboxFocused` bit — good, but we can't detect the trading post input fields.

### 4.5 Config schema
- Current format revision: `2`. Legacy `1` is migrated in-place ([RadialContext.cpp:2083-2092](src/Core/RadialContext.cpp#L2083-L2092)).
- **Hard rule from kickoff prompt: do not remove or rename existing JSON fields.** New features add new fields; older packs must continue to load.
- GW2-RadialMenus#2 reported silent pack corruption. Commit `2ac4526` hardened parsing. **Consider adding a `try/catch` around each pack load to isolate failures** — already in place at [RadialContext.cpp:2019-2163](src/Core/RadialContext.cpp#L2019-L2163). Good; no change needed.

### 4.6 Linux / Proton
- GW2-RadialMenus#2 — Linux/Proton modifier-priority issue.
- GW2Radial#367 — multi-monitor / window manager focus bugs.
- **Design rule: Windows-first. Don't break Linux deliberately, but don't invest in fixing it.**

### 4.7 Upstream-vs-fork disposition
From the feature list, candidates to send upstream as PRs:
- Feature 3 (dismount-on-same-mount) — directly requested in GW2-RadialMenus#18.
- Feature 1 (icon size + angular spread) — pure UX, no policy surface.
- Feature 2 reticule — complements existing `DoNotCenterCursor` toggle.
Features that stay in the fork (more opinionated):
- Feature 4 (combat queue) — complex, may conflict with maintainer's conditional-activation philosophy (GW2-RadialMenus#15).
- Feature 5 (smart-skip) — changes activation semantics.
- Feature 6 (fade-in) — subjective; maintainer has not shipped this upstream.

---

## 5. Build / test loop

- Build: `x64 Release` in Visual Studio → `radials.dll` in `x64/Release/`.
- Install: copy to `<GW2>/addons/`; Nexus auto-loads at next launch.
- Hot-reload: with Nexus's in-game addon manager, toggle off/on.
- State sanity: the Debug tab ([RadialContext.cpp:1912](src/Core/RadialContext.cpp#L1912)) already dumps `StateObserver::RenderDebug()` — use this to verify new state bits.
- **In-game verification is mandatory** per project rules; type-check / build is not enough for behavior features.

---

## 6. Open questions / to-revisit

- The existing Action-Cam handling in `CRadialMenu::Activate` threads into `Sleep(10)` twice ([RadialMenu.cpp:417-425](src/Core/RadialMenu.cpp#L417-L425)). Feature 2's reticule timing will interact with this. Need to decide whether to centralize the "open → maybe-toggle-AC → warp cursor → fade-in" sequence.
- Should per-item "is this my current mount?" comparison for Feature 3 live as a new `Conditions` field (`IsCurrentMount`) or as a dedicated `EActionType::DismountIfCurrent`? Former is more data-driven and editor-friendly; latter is more explicit. Ask user before implementing.
- Feature 4's "cancel on movement" requires peeking at `WM_KEYDOWN` for movement keys. We already have a `WndProc` callback — but we need to know the user's **actual** movement bind (which may not be WASD). `APIDefs->GameBinds.IsBound(EGameBinds_MoveForward)` exists but there's no "get the key" accessor until v7. Current plan: detect any `WM_KEYDOWN` / `WM_MOUSEMOVE` as cancel trigger. Needs UX validation.
- Version gating for eventual Nexus v7 rewrite — should we introduce a `NexusShim.h` now or wait until v7 ships? Current recommendation: wait, but keep every Nexus call call-site-grep-able (no macros).

---

## 7. References

- GW2-RadialMenus upstream: https://github.com/RaidcoreGG/GW2-RadialMenus
- GW2Radial upstream: https://github.com/Friendly0Fire/GW2Radial (abandoned, MIT)
- Nexus addon loader: https://github.com/RaidcoreGG/Nexus
- Nexus public API header: https://github.com/RaidcoreGG/RCGG-lib-nexus-api
- RTAPI releases: https://github.com/RaidcoreGG/GW2-RealTime-API-Releases
- ArenaNet macro policy: https://help.guildwars2.com/hc/en-us/articles/360013762153-Policy-Macros-and-Macro-Use
- Key upstream issues to watch: Nexus#91 (v7 rewrite), GW2-RadialMenus#4 (Action Cam), GW2-RadialMenus#17 (airborne flicker), GW2-RadialMenus#18 (dismount feature request).
