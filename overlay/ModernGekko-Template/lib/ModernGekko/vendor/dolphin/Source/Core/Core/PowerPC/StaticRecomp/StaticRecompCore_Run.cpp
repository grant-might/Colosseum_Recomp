// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later
// Modified by Colosseum Recomp on 2026-10-18, see NOTICE.md.

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#if defined(_MSC_VER)
#include <intrin.h>  // __rdtsc for the per-dispatch host-time split
#endif

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);

// rdtsc for the per-dispatch host-time split. Sub-nanosecond resolution is needed because a single
// dispatch is only a few hundred nanoseconds; evaluated only on 1-in-256 dispatches so the probe
// cost stays ~0.1%. Shares across regions are meaningful; the absolute value is TSC ticks.
inline u64 rdtsc_now()
{
#if defined(_MSC_VER)
  return __rdtsc();
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_ia32_rdtsc();
#else
  return 0;
#endif
}

struct FileCloser
{
  void operator()(std::FILE* file) const
  {
    if (file)
      std::fclose(file);
  }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr OpenDispatchTrace()
{
  const char* path = std::getenv("STATICRECOMP_TRACE_FILE");
  if (!path || !*path)
    return {};

  FilePtr file(std::fopen(path, "w"));
  if (file)
  {
    std::fprintf(file.get(), "dispatch,pc,lr,ctr,cr,timebase,ppc_downcount\n");
    std::fflush(file.get());
  }
  return file;
}
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();
  FilePtr dispatch_trace = OpenDispatchTrace();

  // The trace stride. Sampling once per 1,048,576 dispatches is too coarse to see what a single call
  // does: with ~15M calls/second that is one row per 70ms of work. STATICRECOMP_TRACE_STRIDE sets the
  // interval (rounded up to a power of two, default unchanged) so a short run can capture individual
  // dispatches instead of a summary of millions of them.
  const u64 trace_stride_mask = [] {
    const char* setting = std::getenv("STATICRECOMP_TRACE_STRIDE");
    const unsigned long long requested = setting ? std::strtoull(setting, nullptr, 0) : 0ull;
    if (requested < 2ull)
      return static_cast<u64>(0xFFFFFull);
    u64 stride = 1;
    while (stride < requested)
      stride <<= 1;
    return stride - 1;
  }();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);
  const bool lockstep_enabled = m_lockstep_verifier->IsEnabled();
  const auto fast_dispatchable_at = [this](u32 address) {
    if (m_has_rel_modules || !m_forced_fallback_ranges.empty())
      return FastDispatchableAt(address);
    if (!m_module_active || m_chunk_lookup_table.empty())
      return false;

    int lookup_index = -1;
    if (address >= 0x80000000u && address < 0x80000000u + m_lookup_ram_size)
    {
      lookup_index = static_cast<int>((address - 0x80000000u) >> 2);
    }
    else if (address >= 0x90000000u && address < 0x90000000u + m_lookup_exram_size)
    {
      lookup_index = static_cast<int>((m_lookup_ram_size >> 2) + ((address - 0x90000000u) >> 2));
    }
    if (lookup_index < 0 || lookup_index >= static_cast<int>(m_chunk_lookup_table.size()))
      return false;
    const int chunk = m_chunk_lookup_table[lookup_index];
    return chunk >= 0 && m_chunk_state[chunk] == CHUNK_VERIFIED;
  };

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  const auto after_mtmsr = [this](u32 pc) {
    if (pc < 4u || (pc & 3u) != 0)
      return false;

    const u32 instruction_pc = pc - 4u;
    const u8* code = nullptr;
    if (instruction_pc >= 0x80000000u &&
        instruction_pc - 0x80000000u + 4u <= m_guest.ram_size)
    {
      code = m_guest.ram + instruction_pc - 0x80000000u;
    }
    else if (instruction_pc >= 0x90000000u &&
             instruction_pc - 0x90000000u + 4u <= m_guest.exram_size)
    {
      code = m_guest.exram + instruction_pc - 0x90000000u;
    }
    if (!code)
      return false;

    const u32 raw = static_cast<u32>(code[0]) << 24 | static_cast<u32>(code[1]) << 16 |
                    static_cast<u32>(code[2]) << 8 | code[3];
    return (raw & 0xFC0007FEu) == 0x7C000124u;
  };
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit && !m_guest.host_call)
  {
    m_fallback_jit->Run();
    return;
  }

  while (*state_ptr == CPU::State::Running)
  {
    core_timing.Advance();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc) &&
          !(m_guest.host_call && IsHostCallAddress(ppc.pc)) &&
          (m_stall_count <= kModuleStallLimit || ppc.pc != m_stall_pc))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          if (dispatch_trace && (m_native_dispatches & trace_stride_mask) == 0)
          {
            std::fprintf(dispatch_trace.get(), "%llu,%08x,%08x,%08x,%08x,%llu,%d\n",
                         static_cast<unsigned long long>(m_native_dispatches), m_guest.pc,
                         m_guest.lr, m_guest.ctr, m_guest.cr,
                         static_cast<unsigned long long>(m_guest.timebase), ppc.downcount);
            std::fflush(dispatch_trace.get());
          }
          const bool do_ls = lockstep_enabled && m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          if (m_collect_dispatch_samples && (m_native_dispatches & 4095u) == 0)
            ++m_dispatch_samples[m_guest.pc];
          if (m_collect_dispatch_samples)
          {
            // Full per-PC profile to a file, rewritten periodically. The shutdown path reports only the
            // top 16 PCs, and shutdown reports are lost when teardown crashes (0xC000041D, intermittent)
            // - a lesson already paid for twice on this project. Rewriting the whole map every 60s
            // bounds any loss to 60 seconds and keeps every PC, not just the head of the list.
            static std::chrono::steady_clock::time_point pc_dump_last{};
            const auto pc_now = std::chrono::steady_clock::now();
            if (pc_dump_last.time_since_epoch().count() == 0 ||
                pc_now - pc_dump_last >= std::chrono::seconds(60))
            {
              pc_dump_last = pc_now;
              if (const char* pc_path = std::getenv("STATICRECOMP_DISPATCH_FILE"))
              {
                if (FILE* pc_out = std::fopen(pc_path, "wb"))
                {
                  std::fprintf(pc_out, "# pc,samples\ndispatches,%llu\nsites,%llu\n",
                               static_cast<unsigned long long>(m_native_dispatches),
                               static_cast<unsigned long long>(m_dispatch_samples.size()));
                  for (const auto& pc_entry : m_dispatch_samples)
                  {
                    std::fprintf(pc_out, "%08x,%llu\n", pc_entry.first,
                                 static_cast<unsigned long long>(pc_entry.second));
                  }
                  std::fclose(pc_out);
                }
              }
            }
          }
          const bool rd_sample = (m_native_dispatches & 0xFFu) == 0;
          // Keep the body-start mark separate from the running mark: each bucket advances rd_mark,
          // so using it for the total would measure only the tail of the body. That bug made
          // "module" read 1218% of "total" in the first instrumented run.
          const u64 rd_body_start = rd_sample ? rdtsc_now() : 0;
          u64 rd_mark = rd_body_start;
          const u32 runtime_dispatch_address = m_guest.pc;
          u32 linked_dispatch_address = runtime_dispatch_address;
          if (m_has_rel_modules)
            ResolveNativeAddress(runtime_dispatch_address, &linked_dispatch_address, nullptr);
          if (rd_sample)
          {
            const u64 now = rdtsc_now();
            m_rd_resolve += now - rd_mark;
            rd_mark = now;
          }
          m_guest.pc = linked_dispatch_address;
          m_module->dispatch(&m_guest, linked_dispatch_address);
          if (rd_sample)
          {
            const u64 now = rdtsc_now();
            m_rd_module += now - rd_mark;
            rd_mark = now;
          }
          if (m_has_rel_modules)
            m_guest.pc = TranslateRelAddress(m_guest.pc);
          if (rd_sample)
          {
            const u64 now = rdtsc_now();
            m_rd_translate += now - rd_mark;
            rd_mark = now;
          }
          ++m_native_dispatches;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check: CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
          ppc.downcount -= static_cast<int>(effective_charge);
          m_charged_cycles += effective_charge;

          // Stall detection. An earlier revision of this check tested "PC unchanged" alone, which is
          // WRONG: a guest self-branching loop leaves the PC where it was while
          // charging real cycles, and that heuristic disabled the module permanently (both modules
          // dropped to 0 fps). No progress means the PAIR: the PC did not move AND no guest time was
          // charged. Only that combination is a dispatch which accomplished nothing - the burst then
          // charges its 1-cycle floor, CoreTiming refills, and the same address is asked again forever
          // (measured with the LLVM module: 285,540,352 dispatches at one frozen PC, frame_count 0,
          // one core pegged). After a short run of those, stop asking the module at that address and
          // let the interpreter handle it; the gate below re-admits the module as soon as the PC moves.
          if (charge == 0 && m_guest.pc == runtime_dispatch_address)
          {
            if (runtime_dispatch_address == m_stall_pc)
              ++m_stall_count;
            else
            {
              m_stall_pc = runtime_dispatch_address;
              m_stall_count = 1;
            }
          }
          else
          {
            m_stall_pc = 0;
            m_stall_count = 0;
          }
          const u64 total_cycles = m_timebase_cycle_remainder + effective_charge;
          m_guest.timebase += total_cycles / SystemTimers::TIMER_RATIO;
          m_timebase_cycle_remainder = total_cycles % SystemTimers::TIMER_RATIO;
          if (rd_sample)
          {
            const u64 now = rdtsc_now();
            m_rd_timebase += now - rd_mark;
            rd_mark = now;
          }

          // Idle loop skipping for configured target loops (e.g. Wii Menu OSIdleThread)
          if (m_guest.pc == m_idle_pc && m_idle_pc != 0)
          {
            m_system.GetCoreTiming().Idle();
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
          if ((ppc.Exceptions & EXCEPTION_EXTERNAL_INT) != 0 &&
              (m_guest.msr & 0x8000u) != 0 && after_mtmsr(m_guest.pc))
            break;
          if (rd_sample)
          {
            // The loop condition's own work, timed by re-evaluating its two lookups on this
            // dispatch rather than hoisting them out of the short-circuit chain. Same pc the
            // condition below will use, so this measures the real cost at the same state.
            const u64 cond_start = rdtsc_now();
            const bool probe_dispatchable = fast_dispatchable_at(m_guest.pc);
            const bool probe_hostcall = m_guest.host_call && IsHostCallAddress(m_guest.pc);
            const u64 cond_end = rdtsc_now();
            (void)probe_dispatchable;
            (void)probe_hostcall;
            m_rd_cond += cond_end - cond_start;
            m_rd_total += cond_end - rd_body_start;  // whole body; rd_mark has been advanced
            ++m_rd_samples;
          }
        } while (m_module_active && fast_dispatchable_at(m_guest.pc) &&
                 !(m_guest.host_call && IsHostCallAddress(m_guest.pc)) && ppc.downcount > 0 &&
                 (m_stall_count <= kModuleStallLimit || m_guest.pc != m_stall_pc) &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
        else if ((ppc.Exceptions & EXCEPTION_EXTERNAL_INT) != 0 && ppc.msr.EE &&
                 after_mtmsr(ppc.pc))
          power_pc.CheckExternalExceptions();
      }
      else
      {
        if (m_guest.host_call && IsHostCallAddress(ppc.pc))
        {
          SyncIn();
          const u64 rd_hook_start = rdtsc_now();
          bool handled = m_guest.host_call(&m_guest, m_guest.pc);
          if (!handled && m_guest.pc < m_guest.ram_size)
            handled = m_guest.host_call(&m_guest, m_guest.pc | 0x80000000u);
          m_rd_hook += rdtsc_now() - rd_hook_start;
          if (m_fallback_jit && IsHostCallAddress(m_guest.lr))
            m_fallback_jit->GetBlockCache()->InvalidateICache(m_guest.lr, 4, true);
          if (handled)
          {
            const s64 charge = -m_guest.downcount;
            m_guest.downcount = 0;
            const u64 effective_charge = static_cast<u64>(charge > 0 ? charge : 1);
            ppc.downcount -= static_cast<int>(effective_charge);
            AdvanceGuestTimebase(effective_charge);
            SyncOut();
            continue;
          }
          SyncOut();
          if (m_fallback_jit)
          {
            m_host_call_passthrough_pc = ppc.pc;
            m_host_call_passthrough = true;
          }
        }
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        if (m_module_active && IsForcedFallbackAddress(ppc.pc))
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
        }
        else if (m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else
        {
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) &&
                   !IsHostCallAddress(ppc.pc) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
