# RobinStackKiller

**One-off Starfield/SFSE recovery plugin** for the serialized runaway Papyrus stack:

`unocRobinCheckSpecialistEffectScript.OnInit`

This is intentionally *not* a general Papyrus cleaner.

## Why this exists

The original Robin script contains a loop over `foundSpecialists` but never increments `I`. Once that `OnInit()` execution was serialized into a save, replacing the PEX or removing the magic effect did not necessarily remove the already-running VM stack. In the affected save the stack later points at a dead `FF...` actor/native object and repeatedly calls `HasPerk()`.

## What the plugin does

It scans the live Papyrus VM for stacks containing an exact frame match:

- object/script type: `unocRobinCheckSpecialistEffectScript`
- function/event: `OnInit`

It does **not** target a FormID, Robin's alias, every ActiveMagicEffect, or all Papyrus stacks.

Recovery is staged:

1. **Soft escape:** on the first sighting it moves only the matching `OnInit` frame's instruction pointer to the end of that function, giving the VM a chance to unwind it normally.
2. **Hard finish:** if the exact same stack is still running on a later full scan, it marks only that stack `kFinished` and leaves ownership/cleanup to the VM.
3. It waits for two full clean scans and then stops doing work.

It deliberately does **not** erase the VM stack map, free tasklets, null callbacks, remove quest aliases, or call `DropAllRunningData()`.

## Before using it

1. **Back up the affected save. Do not overwrite it.**
2. Keep your corrected Robin PEX installed. The original loop needs `I += 1` before `EndWhile`.
3. Prefer temporarily removing/disabling `unocRobinCheckSpecialistEffect [MGEF:FE0372D7]` from `unocRobinSpellBarks [SPEL:FE03716E]` while doing the recovery, so the game cannot create a fresh copy of the broken effect during testing.
4. This directly changes internal Papyrus stack state. It is an experimental repair tool, not a normal gameplay plugin.

## Requirements to build

- Visual Studio 2022 C++ Build Tools or another C++23-capable MSVC/Clang-CL setup
- Git
- XMake 3.0.0+
- SFSE runtime for the game

## Build

From PowerShell in this folder:

```powershell
.\build.ps1
```

The script clones the maintained `libxse/commonlibsf` repository with submodules if needed, configures a `releasedbg` x64 build, and builds `RobinStackKiller`.

The DLL should be under a path similar to:

`build\windows\x64\releasedbg\RobinStackKiller.dll`

If your exact XMake generator chooses a slightly different configuration folder, search under `build\windows\` for `RobinStackKiller.dll`.

## Install / recovery procedure

1. Put `RobinStackKiller.dll` in:
   `Starfield\Data\SFSE\Plugins\`
2. Launch Starfield through SFSE.
3. Load a **copy** of the affected save.
4. Wait several seconds. The plugin continuously scans until it sees the serialized target stack.
5. Check the SFSE/CommonLib plugin log for messages beginning with `RobinStackKiller:`.
6. The desired final message is:
   `DONE. target stack no longer running...`
7. Confirm the repeated Robin `HasPerk` / `<savegame>` spam has stopped.
8. Wait another few seconds and **save to a brand-new slot**.
9. Quit Starfield completely.
10. Remove `RobinStackKiller.dll`.
11. Start Starfield again and load the new save. Confirm the runaway stack does not return.
12. Only then restore/re-enable the Robin MGEF if you temporarily removed it, and keep the corrected PEX installed.

## Expected log sequence

Best case:

```text
RobinStackKiller: FOUND target stack=... -- attempting soft escape
RobinStackKiller: SOFT ESCAPE stack=...
RobinStackKiller: clean verification pass 1/2
RobinStackKiller: clean verification pass 2/2
RobinStackKiller: DONE...
```

If the saved execution refuses to unwind normally:

```text
RobinStackKiller: FOUND target stack=... -- attempting soft escape
RobinStackKiller: target stack=... survived soft escape; escalating
RobinStackKiller: HARD FINISH stack=...
...
RobinStackKiller: DONE...
```

## If it finds nothing

Run `dps` / `DumpPapyrusStacks` and verify the current stack still contains the exact script/function names. If the names differ, edit only these constants in `src/main.cpp`:

```cpp
constexpr char kTargetScript[] = "unocRobinCheckSpecialistEffectScript";
constexpr char kTargetFunction[] = "OnInit";
```

Do **not** broaden the matcher to generic `OnInit`, `ActiveMagicEffect`, or every Robin script.

## If it crashes

Use the backup save and remove the DLL. Send the crash log plus the RobinStackKiller log. Do not keep repeatedly saving after a crash or partial repair.

## Important limitation

This source is based on the current maintained CommonLibSF VM layouts/API. I cannot execute or validate the resulting Windows DLL against your Starfield process from this environment. Treat the first run as a controlled recovery test on a copy of the save.
