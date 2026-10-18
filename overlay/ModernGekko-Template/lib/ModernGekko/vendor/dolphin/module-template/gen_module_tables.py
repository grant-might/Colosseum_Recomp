#!/usr/bin/env python3
"""Generate module_tables.inc for a dolphin-chassis static recompilation module.

Parses the generated dispatcher's coverage ranges out of generated.h. Both
the original per-chunk address guards and DolRecomp's compact offset-table
dispatch runs are supported. SMC candidate ranges come from generated_smc.txt,
so the module ABI tables can never drift from a DolRecomp regen.

REL support (--rel-* options)
-----------------------------
A disc has one main.dol plus N REL overlay modules. Each REL is recompiled as
its own DolRecomp output directory, each with its own generated.h. To make the
REL code execute natively the module must describe all of them to the chassis:

  * code_ranges   -- DOL ranges + every REL's compiled range
  * chunk_ranges  -- chunks tiling those ranges exactly (one chunk per REL
                     translation unit, same as the DOL)
  * chunk_hashes  -- FNV-1a 64 of each chunk's original bytes
  * rel_modules   -- per-REL StaticRecompRelModule metadata (module id, version,
                     section count, section-info offset, file size) plus the
                     executable sections, so the chassis can map a REL's runtime
                     load address onto the address it was compiled at

Every REL section's `file offset -> linked address` mapping recorded in the
recompiler's batch log is re-derived from the .rel header and cross-checked;
the generator refuses to emit (and exits non-zero) on any mismatch, so a REL
whose code was compiled for a base the loader will not reproduce can never be
silently mis-mapped at runtime.

The relocation checks also decide the chunk hash policy: RLs are patched by the
loader before they run, so `--rel-hash-mode file` hashes the .rel file bytes
(what the code was compiled from) and `--rel-hash-mode runtime` emits a zero
hash, which tells the chassis to adopt the post-load guest image as the
baseline on first verification (`VerifyChunk` does this when a chunk hash is
zero). `auto` picks `file` when no relocation record targets an executable
section and `runtime` otherwise.
"""
import argparse
import re
import sys
from pathlib import Path

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1

# REL header (version 3 is 0x4C bytes; the section table follows at
# section_info_offset, which the chassis requires to be >= 0x40).
REL_MODULE_ID = 0x00
REL_SECTION_COUNT = 0x0C
REL_SECTION_INFO_OFFSET = 0x10
REL_VERSION = 0x1C
REL_RELOC_OFFSET = 0x24
REL_IMPORT_OFFSET = 0x28
REL_IMPORT_SIZE = 0x2C

# Relocation types that write to the section they name.
RELOC_WRITE_TYPES = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 201, 202, 203}


def fnv1a64(data: bytes) -> int:
    h = FNV64_OFFSET
    for b in data:
        h = ((h ^ b) * FNV64_PRIME) & MASK64
    return h


def be32(data: bytes, off: int) -> int:
    return int.from_bytes(data[off : off + 4], "big")


# --------------------------------------------------------------------------
# DOL
# --------------------------------------------------------------------------
def load_dol_text(dol_path: Path):
    """Return a function mapping a guest address range to the DOL's bytes."""
    dol = dol_path.read_bytes()

    sections = []
    for i in range(18):  # 7 text + 11 data sections share the layout
        file_off = be32(dol, 0x00 + i * 4)
        address = be32(dol, 0x48 + i * 4)
        size = be32(dol, 0x90 + i * 4)
        if file_off and address and size:
            sections.append((address, size, file_off))

    def read_range(start: int, end: int) -> bytes:
        for address, size, file_off in sections:
            if address <= start and end <= address + size:
                lo = file_off + (start - address)
                return dol[lo : lo + (end - start)]
        raise ValueError(f"range [0x{start:08X},0x{end:08X}) not inside one DOL section")

    return read_range


def parse_generated_header(header: str):
    """Coverage ranges + chunk entry addresses from one DolRecomp generated.h."""
    code_ranges = set()
    for a, b in re.findall(
        r"address >= (0x[0-9A-Fa-f]+)u && address < (0x[0-9A-Fa-f]+)u", header
    ):
        code_ranges.add((int(a, 16), int(b, 16)))
    for base, span in re.findall(
        r"u32\s+offset\s*=\s*address\s*-\s*(0x[0-9A-Fa-f]+)u\s*;\s*"
        r"if\s*\(\s*offset\s*<\s*(0x[0-9A-Fa-f]+)u",
        header,
    ):
        start = int(base, 16)
        code_ranges.add((start, start + int(span, 16)))
    func_addrs = sorted(
        int(a, 16)
        for a in re.findall(r"void func_([0-9A-Fa-f]{8})\(CPUState\* ctx\);", header)
    )
    return sorted(code_ranges), func_addrs


def tile_chunks(code_ranges, func_addrs, label):
    """One chunk per generated translation unit, exactly tiling code_ranges."""
    chunk_ranges = []
    for i, addr in enumerate(func_addrs):
        containing = next(((a, b) for a, b in code_ranges if a <= addr < b), None)
        if containing is None:
            raise SystemExit(f"error: {label}: func_{addr:08X} outside all code ranges")
        end = containing[1]
        if i + 1 < len(func_addrs) and containing[0] <= func_addrs[i + 1] < containing[1]:
            end = func_addrs[i + 1]
        chunk_ranges.append((addr, end))
    return chunk_ranges


def parse_smc_ranges(smc_txt: Path):
    smc_ranges = []
    for line in smc_txt.read_text().splitlines():
        m = re.match(r"(0x[0-9A-Fa-f]+)-(0x[0-9A-Fa-f]+)", line.strip())
        if m:
            # File ranges are inclusive instruction addresses; ABI ranges are
            # end-exclusive byte ranges.
            smc_ranges.append((int(m.group(1), 16), int(m.group(2), 16) + 4))
    smc_ranges.sort()
    return smc_ranges


# --------------------------------------------------------------------------
# REL batch log
# --------------------------------------------------------------------------
REL_BLOCK_RE = re.compile(
    r"REL \d+/168: (?P<path>[^\r\n]+)\r?\n=== REL Info ===\r?\n"
    r"module id: (?P<mid>\d+)\r?\nversion: (?P<ver>\d+)\r?\n"
    r"base: (?P<base>0x[0-9A-Fa-f]+)\r?\nentry point: (?P<entry>0x[0-9A-Fa-f]+)"
    r".*?\r?\nsections:\r?\n(?P<sections>(?:  \[[^\r\n]*\r?\n)*)",
    re.S,
)
REL_SECTION_RE = re.compile(
    r"\[(?P<idx>\d+)\] file:(?P<foff>0x[0-9A-Fa-f]+) -> addr:(?P<addr>0x[0-9A-Fa-f]+)"
    r"  size:(?P<size>0x[0-9A-Fa-f]+)(?P<exec> executable)?"
)


def parse_rel_batch_log(log_path: Path):
    text = log_path.read_text(errors="replace")
    rels = []
    for m in REL_BLOCK_RE.finditer(text):
        sections = []
        for s in REL_SECTION_RE.finditer(m.group("sections")):
            sections.append(
                dict(
                    index=int(s.group("idx")),
                    file_offset=int(s.group("foff"), 16),
                    address=int(s.group("addr"), 16),
                    size=int(s.group("size"), 16),
                    executable=bool(s.group("exec")),
                )
            )
        rels.append(
            dict(
                path=m.group("path").strip(),
                name=Path(m.group("path").strip().replace("\\", "/")).name,
                module_id=int(m.group("mid")),
                version=int(m.group("ver")),
                base=int(m.group("base"), 16),
                entry=int(m.group("entry"), 16),
                sections=sections,
            )
        )
    if not rels:
        raise SystemExit(f"error: no REL blocks parsed from {log_path}")
    return rels


def parse_rel_relocations(data: bytes, header: dict):
    """Return the set of section indices that internal relocations write to.

    Internal REL relocations are 8-byte records {u16 offset_delta, u8 type,
    u8 section}; the offset is cumulative across the table. `section` names the
    section the record patches. Only records with a writing type count.
    """
    reloc_offset = header["reloc_offset"]
    if not reloc_offset or reloc_offset >= len(data):
        return {}, 0
    patched = {}
    count = 0
    off = reloc_offset
    while off + 8 <= len(data):
        reloc_type = data[off + 2]
        section = data[off + 3]
        if reloc_type in RELOC_WRITE_TYPES:
            patched[section] = patched.get(section, 0) + 1
        count += 1
        off += 8
    return patched, count


def build_rel_tables(args):
    batch_log = Path(args.rel_batch_log)
    src_dir = Path(args.rel_src_dir)
    out_dir = Path(args.rel_out_dir)
    rels = parse_rel_batch_log(batch_log)

    rel_modules = []
    rel_sections = []
    rel_chunks = []  # (start, end, hash, entry_symbol)
    rel_smc = []
    failures = []
    reloc_stats = []

    for rel in rels:
        rel_path = src_dir / rel["name"]
        if not rel_path.is_file():
            failures.append(f"{rel['name']}: source .rel not found at {rel_path}")
            continue
        data = rel_path.read_bytes()
        file_size = len(data)

        header_mid = be32(data, REL_MODULE_ID)
        section_count = be32(data, REL_SECTION_COUNT)
        section_info_offset = be32(data, REL_SECTION_INFO_OFFSET)
        version = be32(data, REL_VERSION)
        reloc_offset = be32(data, REL_RELOC_OFFSET)

        if header_mid != rel["module_id"]:
            failures.append(
                f"{rel['name']}: header module id {header_mid} != log {rel['module_id']}"
            )
            continue
        if version != rel["version"]:
            failures.append(f"{rel['name']}: header version {version} != log {rel['version']}")
            continue
        if section_info_offset < 0x40:
            failures.append(
                f"{rel['name']}: section_info_offset 0x{section_info_offset:X} < 0x40 "
                f"(chassis rejects it)"
            )
            continue
        if file_size < 0x40:
            failures.append(f"{rel['name']}: file_size 0x{file_size:X} < 0x40")
            continue
        if section_info_offset + section_count * 8 > file_size:
            failures.append(f"{rel['name']}: section table runs past end of file")
            continue

        # Header section table -> (file offset, executable, size).
        header_sections = []
        for i in range(section_count):
            raw = be32(data, section_info_offset + 8 * i)
            size = be32(data, section_info_offset + 8 * i + 4)
            header_sections.append((raw & ~1, bool(raw & 1), size))

        # Cross-check the log's file offset -> linked address mapping for EVERY
        # section (not just executable ones). Compile-time linked address must be
        # base + file offset or the runtime address translation is wrong.
        for section in rel["sections"]:
            if section["file_offset"] >= file_size:
                failures.append(
                    f"{rel['name']}: section [{section['index']}] file offset "
                    f"0x{section['file_offset']:X} past end of file (0x{file_size:X})"
                )
                continue
            expected = rel["base"] + section["file_offset"]
            if section["address"] != expected:
                failures.append(
                    f"{rel['name']}: section [{section['index']}] log addr "
                    f"0x{section['address']:08X} != base 0x{rel['base']:08X} + file offset "
                    f"0x{section['file_offset']:X} = 0x{expected:08X}"
                )
            idx = section["index"]
            if idx >= section_count:
                failures.append(
                    f"{rel['name']}: log section index {idx} >= header section count "
                    f"{section_count}"
                )
            elif header_sections[idx] != (
                section["file_offset"],
                section["executable"],
                section["size"],
            ):
                failures.append(
                    f"{rel['name']}: section [{idx}] header "
                    f"(off=0x{header_sections[idx][0]:X} exec={header_sections[idx][1]} "
                    f"size=0x{header_sections[idx][2]:X}) != log "
                    f"(off=0x{section['file_offset']:X} exec={section['executable']} "
                    f"size=0x{section['size']:X})"
                )

        # Relocation scan: does the loader patch the executable section?
        patched, reloc_count = parse_rel_relocations(data, dict(reloc_offset=reloc_offset))
        exec_indices = [s["index"] for s in rel["sections"] if s["executable"]]
        exec_patched = sorted(i for i in exec_indices if patched.get(i))
        reloc_stats.append((rel["name"], reloc_count, exec_patched, patched))

        # Per-REL DolRecomp output.
        module_dir = out_dir / f"{Path(rel['name']).stem}_{rel['module_id']}"
        gen_h = module_dir / "generated.h"
        gen_smc = module_dir / "generated_smc.txt"
        if not gen_h.is_file():
            failures.append(f"{rel['name']}: missing {gen_h}")
            continue
        header_text = gen_h.read_text()
        rel_code_ranges, rel_func_addrs = parse_generated_header(header_text)
        if not rel_code_ranges or not rel_func_addrs:
            failures.append(f"{rel['name']}: no code ranges or chunk funcs in {gen_h}")
            continue
        rel_chunks_here = tile_chunks(rel_code_ranges, rel_func_addrs, rel["name"])

        # The compiled range must equal an executable section of the .rel, and
        # the chunk entry must sit at its start.
        log_exec = [s for s in rel["sections"] if s["executable"]]
        if len(log_exec) != len(rel_code_ranges):
            failures.append(
                f"{rel['name']}: {len(rel_code_ranges)} compiled range(s) but "
                f"{len(log_exec)} executable section(s) in the log"
            )
            continue
        for (start, end), section in zip(rel_code_ranges, log_exec):
            if (start, end) != (section["address"], section["address"] + section["size"]):
                failures.append(
                    f"{rel['name']}: compiled range [0x{start:08X},0x{end:08X}) != section "
                    f"[{section['index']}] [0x{section['address']:08X},"
                    f"0x{section['address'] + section['size']:08X})"
                )

        # Chunk hashes.
        use_runtime_hash = args.rel_hash_mode == "runtime" or (
            args.rel_hash_mode == "auto" and exec_patched
        )
        for (start, end) in rel_chunks_here:
            entry_symbol = f"func_{start:08X}"
            if use_runtime_hash:
                # Zero => the chassis adopts the post-load guest image as the
                # baseline on first verification. Measured: for these RELs the
                # executable section is relocated, so the learned baseline can be
                # a mid-load image and the chunk then runs code the module was not
                # compiled from. Kept only as an explicit opt-in.
                chunk_hash = 0
            else:
                lo = start - rel["base"]
                if not (0 <= lo and end - rel["base"] <= file_size):
                    failures.append(
                        f"{rel['name']}: chunk [0x{start:08X},0x{end:08X}) maps outside the "
                        f".rel file"
                    )
                    continue
                chunk_hash = fnv1a64(data[lo : end - rel["base"]])
            rel_chunks.append((start, end, chunk_hash, entry_symbol))

        # REL module metadata: only executable sections are described.
        first_section = len(rel_sections)
        for (start, _end), section in zip(rel_code_ranges, log_exec):
            rel_sections.append(
                dict(
                    module_id=rel["module_id"],
                    section_index=section["index"],
                    linked_start=start,
                    size=section["size"],
                )
            )
        rel_modules.append(
            dict(
                name=rel["name"],
                module_id=rel["module_id"],
                version=version,
                section_count=section_count,
                section_info_offset=section_info_offset,
                file_size=file_size,
                first_section=first_section,
                num_sections=len(rel_code_ranges),
            )
        )

        if gen_smc.is_file():
            rel_smc.extend(parse_smc_ranges(gen_smc))

    if failures:
        print("error: REL table generation refused:", file=sys.stderr)
        for failure in failures[:40]:
            print(f"  - {failure}", file=sys.stderr)
        if len(failures) > 40:
            print(f"  ... and {len(failures) - 40} more", file=sys.stderr)
        raise SystemExit(1)

    patched_any = [s for s in reloc_stats if s[2]]
    return (
        rel_modules,
        rel_sections,
        rel_chunks,
        sorted(rel_smc),
        reloc_stats,
        patched_any,
    )


# --------------------------------------------------------------------------
# emission
# --------------------------------------------------------------------------
def emit_range_table(out, name, ranges, count_macro):
    out.write(f"static const StaticRecompRange {name}[] = {{\n")
    if ranges:
        for a, b in ranges:
            out.write(f"    {{0x{a:08X}u, 0x{b:08X}u}},\n")
    else:
        out.write("    {0u, 0u}, /* C and MSVC do not support zero-sized arrays. */\n")
    out.write("};\n")
    out.write(f"#define {count_macro} {len(ranges)}u\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated_h")
    parser.add_argument("smc_txt")
    parser.add_argument("dol_path")
    parser.add_argument("out_path")
    parser.add_argument("--rel-batch-log", default="")
    parser.add_argument("--rel-src-dir", default="")
    parser.add_argument("--rel-out-dir", default="")
    parser.add_argument(
        "--rel-hash-mode",
        choices=["file", "runtime", "auto"],
        default="file",
        help="file: hash the .rel bytes (default, and the only mode measured "
        "safe -- a zero hash makes the chassis adopt whatever guest RAM holds "
        "at first verification as the baseline, and for a REL whose executable "
        "section is relocated that can be a mid-load image, after which the "
        "chunk is dispatched on code the module was not compiled from); "
        "runtime: emit zero (the ABI's runtime-learned baseline); "
        "auto: file when no relocation targets an executable section, else runtime",
    )
    args = parser.parse_args()

    generated_h = Path(args.generated_h)
    smc_txt = Path(args.smc_txt)
    dol_path = Path(args.dol_path)
    out_path = Path(args.out_path)

    header = generated_h.read_text()
    code_ranges, func_addrs = parse_generated_header(header)
    if not code_ranges:
        print("error: no coverage ranges found in", generated_h, file=sys.stderr)
        return 1
    if not func_addrs:
        print("error: no func_ declarations found in", generated_h, file=sys.stderr)
        return 1
    dol_chunks = tile_chunks(code_ranges, func_addrs, "DOL")

    read_dol = load_dol_text(dol_path)
    chunks = [(a, b, fnv1a64(read_dol(a, b))) for a, b in dol_chunks]
    smc_ranges = parse_smc_ranges(smc_txt)

    rel_modules = []
    rel_sections = []
    rel_chunks = []
    reloc_stats = []
    patched_any = []
    has_rels = bool(args.rel_batch_log and args.rel_src_dir and args.rel_out_dir)
    if has_rels:
        (
            rel_modules,
            rel_sections,
            rel_chunks,
            rel_smc,
            reloc_stats,
            patched_any,
        ) = build_rel_tables(args)
        for rel_chunk in rel_chunks:
            chunks.append((rel_chunk[0], rel_chunk[1], rel_chunk[2]))
        smc_ranges = sorted(smc_ranges + rel_smc)

    # ---- merged invariants the chassis enforces at load time ----
    # RangesAreSorted: sorted and non-overlapping.
    all_ranges = sorted(code_ranges + [
        (r["linked_start"], r["linked_start"] + r["size"]) for r in rel_sections
    ])
    for i in range(1, len(all_ranges)):
        if all_ranges[i][0] < all_ranges[i - 1][1]:
            raise SystemExit(
                f"error: code ranges overlap: [0x{all_ranges[i-1][0]:08X},"
                f"0x{all_ranges[i-1][1]:08X}) and [0x{all_ranges[i][0]:08X},"
                f"0x{all_ranges[i][1]:08X})"
            )
    chunks.sort(key=lambda c: c[0])

    # ChunksTileCode: chunk ranges exactly tile code ranges, no gaps/overlaps.
    # Mirrors the chassis algorithm, including its single chunk cursor shared
    # across all code ranges.
    chunk_cursor_index = 0
    for a, b in all_ranges:
        cursor = a
        while (
            chunk_cursor_index < len(chunks)
            and chunks[chunk_cursor_index][0] < b
        ):
            start, end, _h = chunks[chunk_cursor_index]
            if start != cursor:
                raise SystemExit(
                    f"error: chunk tiling gap at 0x{cursor:08X} inside code range "
                    f"[0x{a:08X},0x{b:08X}) (next chunk starts at 0x{start:08X})"
                )
            if end > b:
                raise SystemExit(
                    f"error: chunk [0x{start:08X},0x{end:08X}) overruns code range end "
                    f"0x{b:08X}"
                )
            cursor = end
            chunk_cursor_index += 1
        if cursor != b:
            raise SystemExit(
                f"error: chunk tiling does not reach end of code range "
                f"[0x{a:08X},0x{b:08X}) (stopped at 0x{cursor:08X})"
            )
    if chunk_cursor_index != len(chunks):
        raise SystemExit(
            f"error: {len(chunks) - chunk_cursor_index} chunk(s) fall outside every code range"
        )

    # RelModulesValid: every described section must be inside code_ranges.
    for section in rel_sections:
        start = section["linked_start"]
        end = start + section["size"]
        covered = any(a <= start and end - 1 < b for a, b in all_ranges)
        if not covered:
            raise SystemExit(
                f"error: REL {section['module_id']} section {section['section_index']} "
                f"[0x{start:08X},0x{end:08X}) is not inside any code range"
            )
        if section["module_id"] == 0 or section["size"] == 0:
            raise SystemExit("error: zero module id or section size")
    for module in rel_modules:
        if (
            module["module_id"] == 0
            or module["section_count"] == 0
            or module["section_info_offset"] < 0x40
            or module["file_size"] < 0x40
            or module["num_sections"] == 0
        ):
            raise SystemExit(
                f"error: REL module {module['name']} metadata fails the chassis "
                f"RelModulesValid checks"
            )

    # ---- emit ----
    with out_path.open("w") as f:
        f.write("// Generated by gen_module_tables.py -- do not edit.\n")
        emit_range_table(f, "s_code_ranges", all_ranges, "MODULE_CODE_RANGE_COUNT")
        emit_range_table(f, "s_smc_ranges", smc_ranges, "MODULE_SMC_RANGE_COUNT")
        emit_range_table(
            f, "s_chunk_ranges", [(c[0], c[1]) for c in chunks], "MODULE_CHUNK_RANGE_COUNT"
        )
        f.write("static const u64 s_chunk_hashes[] = {\n")
        for _a, _b, h in chunks:
            f.write(f"    0x{h:016X}u,\n")
        f.write("};\n")

        # ---- REL module metadata ----
        f.write("\n/* REL module metadata: what the chassis needs to map a loaded\n"
                "   REL's runtime address onto the address it was compiled at. */\n")
        f.write("#define MODULE_REL_MODULE_COUNT %du\n" % len(rel_modules))
        f.write("#define MODULE_REL_SECTION_COUNT %du\n" % len(rel_sections))
        f.write("#define MODULE_HAS_REL_MODULES %du\n" % (1 if rel_modules else 0))
        f.write("static const StaticRecompRelSection s_rel_sections[] = {\n")
        if rel_sections:
            for s in rel_sections:
                f.write(
                    f"    {{{s['module_id']}u, {s['section_index']}u, "
                    f"0x{s['linked_start']:08X}u, 0x{s['size']:08X}u}},\n"
                )
        else:
            f.write("    {0u, 0u, 0u, 0u}, /* zero-sized arrays are not portable C */\n")
        f.write("};\n")
        f.write("static const StaticRecompRelModule s_rel_modules[] = {\n")
        if rel_modules:
            for m in rel_modules:
                f.write(
                    f"    {{{m['module_id']}u, {m['version']}u, {m['section_count']}u, "
                    f"0x{m['section_info_offset']:X}u, 0x{m['file_size']:X}u, "
                    f"&s_rel_sections[{m['first_section']}], {m['num_sections']}u}}, "
                    f"/* {m['name']} */\n"
                )
        else:
            f.write("    {0u, 0u, 0u, 0u, 0u, 0, 0u},\n")
        f.write("};\n")

        # ---- REL native dispatch ----
        f.write("\n/* REL native dispatch: linked address -> generated chunk entry. */\n")
        if rel_chunks:
            for start, _end, _h, symbol in rel_chunks:
                f.write(f"void {symbol}(CPUState* ctx);\n")
            f.write("typedef void (*ModuleRelEntryFn)(CPUState* ctx);\n")
            emit_range_table(
                f,
                "s_rel_dispatch_ranges",
                [(c[0], c[1]) for c in rel_chunks],
                "MODULE_REL_DISPATCH_COUNT",
            )
            f.write("static const ModuleRelEntryFn s_rel_dispatch_functions[] = {\n")
            for _start, _end, _h, symbol in rel_chunks:
                f.write(f"    {symbol},\n")
            f.write("};\n")
            f.write(
                "#define MODULE_REL_DISPATCH_MIN 0x%08Xu\n"
                % min(c[0] for c in rel_chunks)
            )
            f.write(
                "#define MODULE_REL_DISPATCH_MAX 0x%08Xu\n"
                % max(c[1] for c in rel_chunks)
            )
            f.write(
                "\nstatic int module_dispatch_rel(CPUState* ctx, u32 address)\n"
                "{\n"
                "    if (address < MODULE_REL_DISPATCH_MIN || address >= MODULE_REL_DISPATCH_MAX)\n"
                "        return 0;\n"
                "    u32 lo = 0u;\n"
                "    u32 hi = MODULE_REL_DISPATCH_COUNT;\n"
                "    while (lo < hi)\n"
                "    {\n"
                "        const u32 mid = lo + (hi - lo) / 2u;\n"
                "        if (s_rel_dispatch_ranges[mid].end <= address)\n"
                "            lo = mid + 1u;\n"
                "        else\n"
                "            hi = mid;\n"
                "    }\n"
                "    if (lo >= MODULE_REL_DISPATCH_COUNT || address < s_rel_dispatch_ranges[lo].start)\n"
                "        return 0;\n"
                "    ctx->pc = address;\n"
                "    s_rel_dispatch_functions[lo](ctx);\n"
                "    return 1;\n"
                "}\n"
            )
        else:
            f.write("#define MODULE_REL_DISPATCH_COUNT 0u\n")

    print(
        f"module_tables.inc: {len(all_ranges)} code ranges, "
        f"{len(smc_ranges)} smc ranges, {len(chunks)} chunk ranges (hashed), "
        f"{len(rel_modules)} rel modules, {len(rel_sections)} rel sections, "
        f"{len(rel_chunks)} rel dispatch entries"
    )
    if has_rels:
        print(
            f"  rel hash policy: {args.rel_hash_mode} "
            f"({sum(1 for c in rel_chunks if c[2] == 0)} of {len(rel_chunks)} rel chunks "
            f"use the runtime-learned baseline)"
        )
        print(
            f"  rels with relocations into an executable section: {len(patched_any)}/"
            f"{len(reloc_stats)}"
        )
        for name, count, exec_patched, _patched in patched_any[:10]:
            print(f"    {name}: {count} records, exec sections {exec_patched}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
