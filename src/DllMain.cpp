// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// ClassicAPI. If not, see <https://www.gnu.org/licenses/>.

#include "Common.h"
#include "Game.h"
#include "MinHook.h"
#include "Offsets.h"
#include "debug/Log.h"
#include "event/Custom.h"
#include "nameplate/Walk.h"
#include "player/NameCache.h"
#include "text/InlineTexture.h"
#include "text/InlineTexturePool.h"
#include "texture/Transform.h"

static Game::FrameScript_Initialize_t FrameScript_Initialize_o = nullptr;
static Game::LoadScriptFunctions_t LoadScriptFunctions_o = nullptr;
static Game::LoadGlueScriptFunctions_t LoadGlueScriptFunctions_o = nullptr;

// `Frame::RegisterEvent` is `__thiscall(this, eventName)`. MSVC can't emit
// __thiscall on free functions, but a __fastcall function with a dummy EDX
// arg matches the same register layout (ECX=this, EDX unused, stack=name).
using FrameRegisterEvent_t = void(__fastcall *)(void *frame, void *edx,
                                                 const char *eventName);
static FrameRegisterEvent_t FrameRegisterEvent_o = nullptr;

namespace {

// VanillaFixes loads every entry in dlls.txt while the process is suspended,
// then calls an optional exported Load() later from WoW's main thread. Keep
// the state machine independent of C++ runtime locks so duplicate calls are
// harmless and initialization never runs concurrently.
volatile LONG g_loadState = 0; // 0=new, 1=loading, 2=loaded, -1=failed

void TraceLoad(const char *message) {
    OutputDebugStringA("[ClassicAPI-WoWSilicon] ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    Debug::Log::Printf("[ClassicAPI-WoWSilicon] %s", message);
}

bool InstallHook(uintptr_t address, LPVOID hook, LPVOID *original,
                 const char *name) {
    auto *target = reinterpret_cast<LPVOID>(address);
    MH_STATUS status = MH_CreateHook(target, hook, original);
    if (status != MH_OK) {
        Debug::Log::Printf(
            "[ClassicAPI-WoWSilicon] MH_CreateHook(%s) failed: %s", name,
            MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        Debug::Log::Printf(
            "[ClassicAPI-WoWSilicon] MH_EnableHook(%s) failed: %s", name,
            MH_StatusToString(status));
        return false;
    }

    Debug::Log::Printf("[ClassicAPI-WoWSilicon] hook installed: %s", name);
    return true;
}

DWORD FailLoad(const char *phase) {
    Debug::Log::Printf("[ClassicAPI-WoWSilicon] Load failed: %s", phase);
    OutputDebugStringA("[ClassicAPI-WoWSilicon] Load failed\n");

    // The DLL remains mapped when an exported Load() reports failure, unlike
    // a FALSE return from DllMain. Remove any partial hook set explicitly.
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    InterlockedExchange(&g_loadState, -1);
    return 1;
}

} // namespace

static void __fastcall InvalidFunctionPtrCheck_h() {}

static bool __fastcall FrameScript_Initialize_h() {
    // BEFORE the engine tears down the old event table (at the start of
    // FrameScript_Initialize), invalidate our cached slot indices. The
    // table is rebuilt at a fresh allocation; the old slots are stale.
    Event::Custom::PrepareForReload();

    // Clear the nameplate diff state so currently-visible plates
    // refire CREATED and UNIT_ADDED on the next tick — re-presents
    // them to the freshly reloaded UI, matching modern WoW. The
    // wrappers themselves now live in the engine's own registry
    // (via `FrameScript_Object::ScriptRegister`), which the engine
    // tears down and rebuilds across the Lua reset — no per-module
    // cache to clear here.
    NamePlate::Events::PrepareForReload();

    // The reload teardown destroys every icon pool texture, fontstring,
    // and text node — forget all inline-icon pointers now so nothing
    // touches them (see InlineTexture.h / InlineTexturePool.h).
    Text::InlineTexture::PrepareForReload();
    Text::InlineTexturePool::PrepareForReload();

    // Drop per-region corner transforms (SetRotation / SetVertexOffset) — the
    // reload destroys every addon texture and the region pool reuses their pointers.
    Texture::Transform::PrepareForReload();

    // Persist the name cache before the engine starts tearing down.
    // This hook fires on both `/reload` and `/logout` (the engine
    // re-initializes Lua state in both cases), giving us a clean
    // deterministic save point for the common lifecycle events.
    Player::NameCache::Flush();

    FrameScript_Initialize_o();
    return true;
}

static void __fastcall LoadScriptFunctions_h() {
    LoadScriptFunctions_o();
    Game::RunModuleRegistrations();
    // Permit `Event::Custom::TryClaim` to actually write to the event
    // table from here on. Earlier writes (during the engine's own
    // boot-time `RegisterEvent` flurry, plus SuperWoWhook/etc.) can
    // race with the engine's table init and trigger `SMemFree` on slots
    // it still considers in-flight.
    Event::Custom::EnableWrites();
}

static void __stdcall LoadGlueScriptFunctions_h() {
    LoadGlueScriptFunctions_o();
    Game::RunGlueModuleRegistrations();
    // World→glue teardown destroyed every world fontstring and text node the
    // inline-icon maps reference; forget them now that the glue UI is booting.
    // (Glue→world is covered by FrameScript_Initialize_h; the node-free hook
    // handles individual deaths, but bulk teardown paths may bypass it.)
    Text::InlineTexture::PrepareForReload();
    Text::InlineTexturePool::PrepareForReload();
    Texture::Transform::PrepareForReload();
}

// Every Lua-side `frame:RegisterEvent(...)` is a chance to claim a slot
// for any custom event still waiting. By this point the engine's table
// is fully populated and SuperWoWhook / other DLLs have done their
// post-rebuild writes, so the table state is settled and our backwards
// walk finds genuine NULL slots near the tail.
static void __fastcall FrameRegisterEvent_h(void *frame, void *edx,
                                            const char *eventName) {
    Event::Custom::RetryClaims();
    // Throttled-internally; no-op when the cache or scan toggle is off.
    Player::NameCache::Tick();
    FrameRegisterEvent_o(frame, edx, eventName);
}

// VanillaFixes contract: GetProcAddress(module, "Load") and call it on the
// main thread after every additional DLL has left DllMain. Zero means success.
extern "C" __declspec(dllexport) DWORD Load() {
    const LONG previous = InterlockedCompareExchange(&g_loadState, 1, 0);
    if (previous == 2)
        return 0;
    if (previous != 0) {
        TraceLoad(previous == 1 ? "duplicate Load while initialization is active"
                                : "duplicate Load after initialization failed");
        return 1;
    }

    TraceLoad("Load begin (outside DllMain, on loader main thread)");

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        Debug::Log::Printf(
            "[ClassicAPI-WoWSilicon] MH_Initialize failed: %s",
            MH_StatusToString(status));
        InterlockedExchange(&g_loadState, -1);
        return 1;
    }
    TraceLoad("MinHook initialized");

    if (!InstallHook(
            Offsets::FUN_INVALID_FUNCTION_PTR_CHECK,
            reinterpret_cast<LPVOID>(InvalidFunctionPtrCheck_h), nullptr,
            "InvalidFunctionPtrCheck"))
        return FailLoad("InvalidFunctionPtrCheck");

    if (!InstallHook(
            Offsets::FUN_FRAME_SCRIPT_INITIALIZE,
            reinterpret_cast<LPVOID>(FrameScript_Initialize_h),
            reinterpret_cast<LPVOID *>(&FrameScript_Initialize_o),
            "FrameScript_Initialize"))
        return FailLoad("FrameScript_Initialize");

    if (!InstallHook(
            Offsets::FUN_LOAD_SCRIPT_FUNCTIONS,
            reinterpret_cast<LPVOID>(LoadScriptFunctions_h),
            reinterpret_cast<LPVOID *>(&LoadScriptFunctions_o),
            "LoadScriptFunctions"))
        return FailLoad("LoadScriptFunctions");

    if (!InstallHook(
            Offsets::FUN_LOAD_GLUE_SCRIPT_FUNCTIONS,
            reinterpret_cast<LPVOID>(LoadGlueScriptFunctions_h),
            reinterpret_cast<LPVOID *>(&LoadGlueScriptFunctions_o),
            "LoadGlueScriptFunctions"))
        return FailLoad("LoadGlueScriptFunctions");

    if (!InstallHook(
            Offsets::FUN_FRAME_REGISTER_EVENT,
            reinterpret_cast<LPVOID>(FrameRegisterEvent_h),
            reinterpret_cast<LPVOID *>(&FrameRegisterEvent_o),
            "FrameRegisterEvent"))
        return FailLoad("FrameRegisterEvent");

    TraceLoad("core hooks installed");

    // All feature hooks declared via Game::HookAutoRegister at file scope in
    // their respective modules.
    if (!Game::RunHookRegistrations())
        return FailLoad("feature hook registration");

    TraceLoad("feature hooks installed");
    InterlockedExchange(&g_loadState, 2);
    TraceLoad("Load complete");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
