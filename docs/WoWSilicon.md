# WoWSilicon deferred-load build

This branch is a focused compatibility experiment for ClassicAPI under
WoWSilicon/Wine/Rosetta. It does not change any ClassicAPI feature or game
offset. It only changes when MinHook and the ClassicAPI hooks are installed.

VanillaFixes first maps every DLL listed in `dlls.txt`, then calls an optional
export named `Load` from WoW's main thread. This build uses that entrypoint and
keeps `DllMain` limited to `DisableThreadLibraryCalls`.

The required test baseline is:

```text
mods/libSiliconPatch.dll
mods/winerosetta.dll
mods/ClassicAPI-WoWSilicon.dll
```

Do not remove `libSiliconPatch.dll` or `winerosetta.dll`, and do not change
their order while evaluating this build. Compare only the original
`ClassicAPI.dll` with this branch's DLL.

Initialization diagnostics are appended to:

```text
Logs/classicapi_debug.log
```

A successful initialization ends with:

```text
[ClassicAPI-WoWSilicon] Load complete
```

For runtime bisection, set `CLASSICAPI_WOWSILICON_HOOKS` before launching:

| Value | Behavior |
| --- | --- |
| `all` or unset | Install the core and feature hooks. |
| `core` | Install only the five core initialization hooks. |
| `loader-only` | Exercise DLL mapping and `Load()` without initializing MinHook or installing hooks. |

The selected mode is written to the diagnostic log. These modes allow the
WoWSilicon test to isolate a hook-related deadlock without rebuilding or
changing `dlls.txt`.

The DLL is still a 32-bit Windows binary. Build it on a Windows runner with:

```powershell
git submodule update --init --recursive
cmake -B build -A Win32
cmake --build build --config Release
```

The output is `build/Release/ClassicAPI.dll`; rename the copied test artifact
to `ClassicAPI-WoWSilicon.dll` so the original remains available for the
control runs.
