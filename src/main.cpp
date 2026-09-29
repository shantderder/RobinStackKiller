// SPDX-License-Identifier: GPL-3.0-or-later
// One-off recovery plugin for a serialized runaway Papyrus stack.
// Target: unocRobinCheckSpecialistEffectScript.OnInit
//
// v0.2: Do NOT touch instruction pointers or stack state.
// We only locate the target frame's local integer variable named "I" and
// set it to INT_MAX so the saved bytecode's While condition becomes false
// and the VM can unwind the function naturally.

#include "pch.h"

namespace
{
    constexpr char kTargetScript[] = "unocRobinCheckSpecialistEffectScript";
    constexpr char kTargetFunction[] = "OnInit";
    constexpr char kLoopIndexName[] = "I";

    constexpr std::uint32_t kIDsPerTick = 16384;
    constexpr std::uint32_t kCleanPassesBeforeDone = 2;
    constexpr std::int32_t kExitLoopValue = INT32_MAX;

    std::uint32_t g_cursor = 0;
    std::uint32_t g_cleanPasses = 0;
    std::uint32_t g_patchedStacks = 0;
    bool g_passSawTarget = false;
    bool g_everFoundTarget = false;
    bool g_done = false;

    std::unordered_set<std::uint32_t> g_patched;
    std::unordered_set<std::uint32_t> g_dumpedNoIndex;

    bool EqualsNoCase(const char* a_lhs, const char* a_rhs)
    {
        return a_lhs && a_rhs && _stricmp(a_lhs, a_rhs) == 0;
    }

    bool IsTargetFrame(RE::BSScript::StackFrame* a_frame)
    {
        if (!a_frame) {
            return false;
        }

        auto* function = a_frame->owningFunction.get();
        if (!function) {
            return false;
        }

        const auto& scriptName = function->GetObjectTypeName();
        const auto& functionName = function->GetName();
        return EqualsNoCase(scriptName.data(), kTargetScript) &&
               EqualsNoCase(functionName.data(), kTargetFunction);
    }

    RE::BSScript::StackFrame* FindTargetFrame(RE::BSScript::Stack* a_stack)
    {
        if (!a_stack) {
            return nullptr;
        }

        for (auto* frame = a_stack->top; frame; frame = frame->previousFrame) {
            if (IsTargetFrame(frame)) {
                return frame;
            }
        }

        return nullptr;
    }

    void DumpFrameVariablesOnce(RE::BSScript::Stack* a_stack, RE::BSScript::StackFrame* a_frame)
    {
        if (!a_stack || !a_frame || !g_dumpedNoIndex.insert(a_stack->stackID).second) {
            return;
        }

        auto* function = a_frame->owningFunction.get();
        if (!function) {
            return;
        }

        REX::WARN(
            "RobinStackKiller: stack={} target frame found but local '{}' was not found. Dumping {} frame slots:",
            a_stack->stackID,
            kLoopIndexName,
            a_frame->size);

        for (std::uint32_t index = 0; index < a_frame->size; ++index) {
            RE::BSFixedString name;
            if (function->GetVarNameForStackIndex(index, name)) {
                REX::WARN("RobinStackKiller:   slot {} name='{}'", index, name.data() ? name.data() : "<null>");
            }
        }
    }

    bool PatchLoopIndexLocked(RE::BSScript::Stack* a_stack, RE::BSScript::StackFrame* a_frame)
    {
        if (!a_stack || !a_frame) {
            return false;
        }

        auto* function = a_frame->owningFunction.get();
        if (!function) {
            return false;
        }

        const auto page = a_frame->GetPageForFrame();

        for (std::uint32_t index = 0; index < a_frame->size; ++index) {
            RE::BSFixedString name;
            if (!function->GetVarNameForStackIndex(index, name)) {
                continue;
            }

            if (!EqualsNoCase(name.data(), kLoopIndexName)) {
                continue;
            }

            auto& variable = a_frame->GetVariable(index, page);
            if (!variable.is<std::int32_t>()) {
                REX::ERROR(
                    "RobinStackKiller: stack={} found local '{}' in slot {} but it is not an Int. Refusing to modify it.",
                    a_stack->stackID,
                    kLoopIndexName,
                    index);
                return false;
            }

            const auto oldValue = RE::BSScript::get<std::int32_t>(variable);
            variable = kExitLoopValue;

            REX::WARN(
                "RobinStackKiller: PATCHED stack={} local '{}' slot {} {} -> {}. VM will exit the While loop naturally.",
                a_stack->stackID,
                kLoopIndexName,
                index,
                oldValue,
                kExitLoopValue);

            return true;
        }

        return false;
    }

    void HandleStack(RE::BSScript::Internal::VirtualMachine* a_vm, RE::BSScript::Stack* a_stack)
    {
        if (!a_vm || !a_stack) {
            return;
        }

        auto* frame = FindTargetFrame(a_stack);
        if (!frame) {
            return;
        }

        g_passSawTarget = true;
        g_everFoundTarget = true;

        if (g_patched.contains(a_stack->stackID)) {
            return;
        }

        // Hold the VM running-stack write lock only while resolving and mutating
        // the frame's local variable. We do not alter frame->ip, stack->state,
        // owningTasklet, queues, callbacks, or the running-stack map.
        {
            RE::BSAutoWriteLock lock(a_vm->runningStacksLock);

            // Re-resolve while locked in case the top frame changed between discovery and lock.
            frame = FindTargetFrame(a_stack);
            if (!frame) {
                return;
            }

            if (PatchLoopIndexLocked(a_stack, frame)) {
                g_patched.insert(a_stack->stackID);
                ++g_patchedStacks;
                return;
            }
        }

        DumpFrameVariablesOnce(a_stack, frame);
    }

    void ScanTick()
    {
        if (g_done) {
            return;
        }

        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }

        const auto upper = vm->nextStackID;
        if (upper == 0) {
            return;
        }

        if (g_cursor >= upper) {
            g_cursor = 0;
        }

        std::uint32_t scanned = 0;

        while (g_cursor < upper && scanned < kIDsPerTick) {
            const auto stackID = g_cursor++;
            ++scanned;

            RE::BSTSmartPointer<RE::BSScript::Stack> stack;
            if (!vm->GetStackByID(stackID, stack) || !stack) {
                continue;
            }

            HandleStack(vm, stack.get());
        }

        if (g_cursor >= upper) {
            if (g_passSawTarget) {
                g_cleanPasses = 0;
            } else if (g_everFoundTarget) {
                ++g_cleanPasses;
                REX::INFO("RobinStackKiller: clean verification pass {}/{}", g_cleanPasses, kCleanPassesBeforeDone);
            }

            g_passSawTarget = false;
            g_cursor = 0;

            if (g_everFoundTarget && g_cleanPasses >= kCleanPassesBeforeDone) {
                g_done = true;
                REX::INFO(
                    "RobinStackKiller: DONE. target frame no longer present. patchedStacks={}. SAVE TO A NEW SLOT, quit, then remove this DLL.",
                    g_patchedStacks);
            }
        }
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);

    REX::INFO("RobinStackKiller 0.2.0 loaded");
    REX::INFO("Target: {}.{}", kTargetScript, kTargetFunction);
    REX::WARN("v0.2 only edits the target frame's local Int '{}'; it does not modify instruction pointers or stack state.", kLoopIndexName);
    REX::WARN("Recovery plugin: use only on a BACKUP of the affected save");

    const auto* tasks = SFSE::GetTaskInterface();
    if (!tasks) {
        REX::ERROR("RobinStackKiller: SFSE task interface unavailable");
        return false;
    }

    tasks->AddPermanentTask([]() { ScanTick(); });
    return true;
}
