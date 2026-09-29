// SPDX-License-Identifier: GPL-3.0-or-later
// One-off recovery plugin for a serialized runaway Papyrus stack.
// Target: unocRobinCheckSpecialistEffectScript.OnInit

#include "pch.h"

namespace
{
    constexpr char kTargetScript[] = "unocRobinCheckSpecialistEffectScript";
    constexpr char kTargetFunction[] = "OnInit";

    // Scan a bounded number of stack IDs per SFSE task tick so we do not stall a frame
    // on a save with a very large Papyrus stack-ID counter.
    constexpr std::uint32_t kIDsPerTick = 16384;

    // First sighting: move the target frame's IP to the end of the function and let the VM
    // unwind it normally. If the same live stack survives to a later pass, escalate by
    // marking ONLY that exact stack finished.
    constexpr std::uint32_t kHardFinishOnSighting = 2;
    constexpr std::uint32_t kCleanPassesBeforeDone = 2;

    std::uint32_t g_cursor = 0;
    std::uint32_t g_cleanPasses = 0;
    std::uint32_t g_softEscapes = 0;
    std::uint32_t g_hardFinishes = 0;
    bool g_passSawActiveTarget = false;
    bool g_everFoundTarget = false;
    bool g_done = false;
    std::unordered_map<std::uint32_t, std::uint32_t> g_sightings;

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
        return EqualsNoCase(scriptName.data(), kTargetScript) && EqualsNoCase(functionName.data(), kTargetFunction);
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

    void SoftEscape(RE::BSScript::Internal::VirtualMachine* a_vm, RE::BSScript::Stack* a_stack)
    {
        if (!a_vm || !a_stack) {
            return;
        }

        RE::BSAutoWriteLock lock(a_vm->runningStacksLock);
        for (auto* frame = a_stack->top; frame; frame = frame->previousFrame) {
            if (IsTargetFrame(frame)) {
                const auto oldIP = frame->ip;
                frame->ip = frame->size;
                REX::WARN("RobinStackKiller: SOFT ESCAPE stack={} OnInit ip {} -> {} (frame size)", a_stack->stackID, oldIP, frame->size);
            }
        }
        ++g_softEscapes;
    }

    void HardFinish(RE::BSScript::Internal::VirtualMachine* a_vm, RE::BSScript::Stack* a_stack)
    {
        if (!a_vm || !a_stack) {
            return;
        }

        RE::BSAutoWriteLock lock(a_vm->runningStacksLock);

        // Push every target OnInit frame to EOF as well, then mark only this exact stack
        // finished. We deliberately do NOT erase the VM's running-stack map, free tasklets,
        // clear callbacks, or call DropAllRunningData(). The VM keeps ownership and can do
        // its normal cleanup.
        for (auto* frame = a_stack->top; frame; frame = frame->previousFrame) {
            if (IsTargetFrame(frame)) {
                frame->ip = frame->size;
            }
        }

        const auto oldState = a_stack->state;
        a_stack->state = RE::BSScript::Stack::State::kFinished;
        ++g_hardFinishes;

        REX::ERROR(
            "RobinStackKiller: HARD FINISH stack={} oldState={} stackType={} frames={}",
            a_stack->stackID,
            static_cast<std::int32_t>(oldState),
            static_cast<std::int32_t>(a_stack->stackType),
            a_stack->frames);
    }

    void HandleTargetStack(RE::BSScript::Internal::VirtualMachine* a_vm, RE::BSScript::Stack* a_stack)
    {
        if (!a_vm || !a_stack || !FindTargetFrame(a_stack)) {
            return;
        }

        // Finished stacks can linger briefly until the VM cleans them up. Do not count those
        // as an active runaway stack.
        if (a_stack->state == RE::BSScript::Stack::State::kFinished) {
            return;
        }

        g_passSawActiveTarget = true;
        g_everFoundTarget = true;

        auto& seen = g_sightings[a_stack->stackID];
        ++seen;

        if (seen == 1) {
            auto* frame = FindTargetFrame(a_stack);
            REX::WARN(
                "RobinStackKiller: FOUND target stack={} state={} stackType={} OnInit ip={}/{} -- attempting soft escape",
                a_stack->stackID,
                static_cast<std::int32_t>(a_stack->state),
                static_cast<std::int32_t>(a_stack->stackType),
                frame ? frame->ip : 0,
                frame ? frame->size : 0);
            SoftEscape(a_vm, a_stack);
        } else if (seen >= kHardFinishOnSighting) {
            REX::ERROR("RobinStackKiller: target stack={} survived soft escape; escalating", a_stack->stackID);
            HardFinish(a_vm, a_stack);
        }
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

            HandleTargetStack(vm, stack.get());
        }

        // A complete pass has ended. Once we have seen the target at least once, require two
        // full clean passes before disabling the scanner. This also catches duplicate runaway
        // OnInit stacks if the save contains more than one.
        if (g_cursor >= upper) {
            if (g_passSawActiveTarget) {
                g_cleanPasses = 0;
            } else if (g_everFoundTarget) {
                ++g_cleanPasses;
                REX::INFO("RobinStackKiller: clean verification pass {}/{}", g_cleanPasses, kCleanPassesBeforeDone);
            }

            g_passSawActiveTarget = false;
            g_cursor = 0;

            if (g_everFoundTarget && g_cleanPasses >= kCleanPassesBeforeDone) {
                g_done = true;
                REX::INFO(
                    "RobinStackKiller: DONE. target stack no longer running. softEscapes={} hardFinishes={}. SAVE TO A NEW SLOT, then remove this DLL.",
                    g_softEscapes,
                    g_hardFinishes);
            }
        }
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);

    REX::INFO("RobinStackKiller 0.1.0 loaded");
    REX::INFO("Target: {}.{}", kTargetScript, kTargetFunction);
    REX::WARN("Recovery plugin: use only on a BACKUP of the affected save");

    const auto* tasks = SFSE::GetTaskInterface();
    if (!tasks) {
        REX::ERROR("RobinStackKiller: SFSE task interface unavailable");
        return false;
    }

    tasks->AddPermanentTask([]() { ScanTick(); });
    return true;
}
