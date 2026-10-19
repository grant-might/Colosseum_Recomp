// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later
// Modified by Colosseum Recomp on 2026-10-18, see NOTICE.md.

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompABI.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"

namespace Core
{
class System;
}

namespace PowerPC
{
struct PowerPCState;
}

namespace StaticRecompLockstep
{
class StaticRecompLockstepVerifier;
}

// Executes statically recompiled per-game native code when the PC is covered by
// a loaded module; falls back to Dolphin's interpreter for everything else.
// With no module loaded this core is exactly an interpreter loop.
class StaticRecompCore : public JitBase
{
public:
  friend class StaticRecompLockstep::StaticRecompLockstepVerifier;

  explicit StaticRecompCore(Core::System& system, StaticRecompModuleSource module_source);
  StaticRecompCore(const StaticRecompCore&) = delete;
  StaticRecompCore(StaticRecompCore&&) = delete;
  StaticRecompCore& operator=(const StaticRecompCore&) = delete;
  StaticRecompCore& operator=(StaticRecompCore&&) = delete;
  ~StaticRecompCore() override;

  void Init() override;
  void Shutdown() override;

  void Run() override;
  void SingleStep() override;
  bool IsModuleActive() const;
  bool DispatchableAt(u32 address);
  bool FastDispatchableAt(u32 address);
  bool IsHostCallAddress(u32 address) const;
  bool ShouldYieldAt(u32 address);

  void ClearCache() override;
  void Jit(u32 em_address) override {}
  bool HandleFault(uintptr_t access_address, SContext* ctx) override { return false; }

  JitBaseBlockCache* GetBlockCache() override { return &m_block_cache; }

  // Gather-pipe and quantize hooks do not only fire from module code: anything
  // the module does not cover runs on m_fallback_jit, a real Jit64/JitArm64.
  // That core reads fifoWriteAddresses at compile time and owns the block that
  // has to be invalidated, so registering against this core instead left the
  // check uncompiled and the hook firing on every gather-pipe store forever.
  JitBase* GetExceptionCheckTarget() override
  {
    return m_fallback_jit ? m_fallback_jit.get() : this;
  }

  void EraseSingleBlock(const JitBlock& block) override {}
  std::vector<MemoryStats> GetMemoryStats() const override { return {}; }
  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override
  {
    return 0;
  }
  const CommonAsmRoutinesBase* GetAsmRoutines() override { return nullptr; }
  const char* GetName() const override { return "StaticRecomp"; }

private:
  // JitBaseBlockCache with no generated blocks; exists so generic
  // icache-invalidation plumbing in JitInterface has a real object to talk
  // to, and to feed every invalidation into the SMC demotion guard (D4).
  class EmptyBlockCache : public JitBaseBlockCache
  {
  public:
    explicit EmptyBlockCache(StaticRecompCore& core) : JitBaseBlockCache(core), m_core(core) {}
    void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override {}

  protected:
    void InvalidateICacheInternal(u32 physical_address, u32 address, u32 length,
                                  bool forced) override
    {
      m_core.OnICacheInvalidate(address, length);
    }

  private:
    StaticRecompCore& m_core;
  };

  void LoadModule();

  // D4 SMC guard, verify-on-entry model. Every chunk starts Unverified; the
  // first native dispatch into it hashes its guest RAM against the module's
  // recorded hash of the original text. An icache invalidation touching a
  // chunk resets it to Unverified (Dolphin invalidates while *loading* code,
  // so invalidation alone must not retire coverage); a hash mismatch (real
  // SMC) marks it Failed, interpreter-only until the next invalidation.
  enum ChunkState : u8
  {
    CHUNK_UNVERIFIED = 0,
    CHUNK_VERIFIED = 1,
    CHUNK_FAILED = 2,
  };

  void OnICacheInvalidate(u32 address, u32 length);
  int ChunkIndexOf(u32 address);
  bool IsForcedFallbackAddress(u32 address) const;
  bool ChunkContainsHostCall(u32 index) const;
  void VerifyChunk(u32 index);
  bool ResolveNativeAddress(u32 runtime_address, u32* linked_address, u32* rel_section_index);
  bool ResolveRuntimeAddress(u32 linked_address, u32* runtime_address) const;
  u32 TranslateRelAddress(u32 linked_address);
  void RefreshRelSections();

  static void SetPPCStateFromGuestState(const CPUState& s, PowerPC::PowerPCState& ppc);

  // D1 state residency: registers live in m_guest while native code runs;
  // full sync at every native-burst boundary.
  void SyncIn();   // Dolphin PowerPCState -> m_guest
  void SyncOut();  // m_guest -> Dolphin PowerPCState
  void AdvanceGuestTimebase(u64 cpu_cycles);

  // CPUState hooks (module -> chassis environment). `cpu->external_user_data`
  // is the StaticRecompCore*.
  static u64 HookExternalRead(CPUState* cpu, u32 ea, u8 size);
  static void HookExternalWrite(CPUState* cpu, u32 ea, u64 value, u8 size);
  static u32 HookExternalRead32(CPUState* cpu, u32 ea, u8 rid);
  static void HookExternalWrite32(CPUState* cpu, u32 ea, u32 value, u8 rid);
  static void* HookExternalPointer(CPUState* cpu, u32 ea, u32 size);
  static u32 HookSPRRead(CPUState* cpu, u16 spr, u32 cia);
  static void HookSPRWrite(CPUState* cpu, u16 spr, u32 value, u32 cia);
  static void HookCacheControl(CPUState* cpu, u8 operation, u32 ea, u32 cia);
  static void HookInstructionFallback(CPUState* cpu, u32 raw, u32 cia);
  static bool HookHostCall(CPUState* cpu, u32 address);

  // Keep Dolphin's MSR-derived state (translation mode, feature flags) in step
  // with the guest MSR before any MMU access or exception delivery.
  void PropagateGuestMSR();

  std::unique_ptr<StaticRecompLockstep::StaticRecompLockstepVerifier> m_lockstep_verifier;

  EmptyBlockCache m_block_cache{*this};

  CPUState m_guest{};
  Common::DynamicLibrary m_library;
  StaticRecompModuleSource m_module_source;
  const StaticRecompModuleDesc* m_module = nullptr;
  bool m_module_active = false;
  u32 m_host_call_passthrough_pc = 0;
  bool m_host_call_passthrough = false;
  std::unique_ptr<JitBase> m_fallback_jit;

  u64 m_native_dispatches = 0;
  u64 m_fallback_steps = 0;
  u64 m_native_exceptions = 0;
  u64 m_hook_fallback_instructions = 0;
  u64 m_timebase_cycle_remainder = 0;
  std::unordered_map<u32, u64> m_dispatch_samples;
  u64 m_bursts = 0;          // SyncIn..SyncOut native runs (diagnostic)
  u64 m_charged_cycles = 0;  // cycles flushed from module charges (diagnostic)

  // Per-dispatch host-time split, rdtsc units, sampled 1-in-256 so probe cost stays ~0.1%.
  // Compare the shares, not the absolutes: TSC ticks, not nanoseconds.
  //   resolve / module / translate / timebase - measured inline, in order, inside the body.
  //   total - the whole dispatch body, ending just BEFORE the loop condition.
  //   other - total minus those four, i.e. the rest of the body (trace/lockstep/sample checks,
  //           exception checks). Derived so nothing has to be hoisted.
  //   cond  - the loop condition's work, measured SEPARATELY by re-evaluating the two pure
  //           lookups on the sampled dispatch. It is deliberately outside "total": hoisting them
  //           out of the short-circuit chain to time them inline would add work and distort the
  //           split. Compare cond to total/sample to see whether the condition is a real cost.
  //           Assumes FastDispatchableAt and IsHostCallAddress are side-effect free lookups.
  // Dispatches that break out early (hook-raised or external-interrupt exceptions) are not
  // sampled, so the split describes the steady state.
  u64 m_rd_samples = 0;
  u64 m_rd_total = 0;
  u64 m_rd_resolve = 0;
  u64 m_rd_module = 0;
  u64 m_rd_translate = 0;
  u64 m_rd_timebase = 0;
  // Time inside host calls made FROM the generated module (framebuffer/EFB copies and friends).
  // Measured because the module bucket is ~95% of dispatch time and host calls happen inside it, so
  // the split alone cannot say whether that 95% is guest code or re-entry cost. Timed unconditionally:
  // host calls are rare next to dispatches (~12% of them), so two rdtsc each is negligible.
  u64 m_rd_hook = 0;
  u64 m_rd_cond = 0;

  // Module stall detection. A dispatch that returns without moving the guest PC would otherwise be
  // re-dispatched forever: the burst charges its 1-cycle floor, CoreTiming refills the budget, and the
  // guest never advances. Measured with the LLVM module: 285,540,352 dispatches at one frozen address
  // (pc/lr/ctr/cr all byte-identical), frame_count 0, one core pegged. After a short run of identical
  // no-progress dispatches the module stops being consulted and the interpreter handles the address.
  static constexpr u32 kModuleStallLimit = 64;
  u32 m_stall_pc = 0;
  u32 m_stall_count = 0;

  // D4 guard state: parallel to m_module->chunk_ranges.
  std::vector<u8> m_chunk_state;
  mutable std::vector<u8> m_chunk_host_call_state;
  std::vector<StaticRecompRange> m_forced_fallback_ranges;
  struct ActiveRelSection
  {
    u32 module_id;
    u32 section_index;
    u32 linked_start;
    u32 runtime_start;
    u32 size;
  };
  std::vector<ActiveRelSection> m_active_rel_sections;
  std::vector<int> m_chunk_rel_sections;
  std::vector<u64> m_effective_chunk_hashes;
  u64 m_rel_mapping_generation = 0;
  // Work counter (native dispatches + interpreted instructions) at which the
  // next REL discovery scan is allowed. 0 = never scanned yet, so the first
  // miss still scans immediately.
  u64 m_rel_scan_work = 0;
  u32 m_failed_chunks = 0;    // chunks currently failing verification (real SMC)
  u64 m_verifications = 0;    // chunk hash checks performed
  u64 m_reverify_events = 0;  // invalidations that reset a chunk to Unverified

  // Lookup table optimization for O(1) chunk searches
  std::vector<int> m_chunk_lookup_table;
  u32 m_lookup_ram_size = 0;
  u32 m_lookup_exram_size = 0;
  int GetAddressLookupIndex(u32 address) const;
  void InitLookupTable(u32 ram_size, u32 exram_size);

  // Dispatch locality: most control transfers stay inside one chunk, so the
  // last hit short-circuits the chunk binary search on the hot path.
  mutable u32 m_last_chunk_index = 0;

  bool m_collect_dispatch_samples = false;
  bool m_has_rel_modules = false;
  u32 m_idle_pc = 0;
};

extern StaticRecompCore* g_static_recomp_core;
u32 StaticRecompShouldYieldAt(u32 address);
