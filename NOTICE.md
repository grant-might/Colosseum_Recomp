# Notice

Colosseum Recomp is a launcher built on other people's work. This file says whose, and what this repo changes.

## Upstream projects

**ModernGekko** and **ModernGekko-Template**, by ExpansionPak.
https://github.com/ExpansionPak/ModernGekko
https://github.com/ExpansionPak/ModernGekko-Template

These supply the recompiler runtime, the frontend framework and the per-game build pipeline, which is most of the code the launcher sits next to. Both trees ship under GPL-3.0. The launcher and runner in `overlay/` are modified copies of files from ModernGekko, and the Makefile is a modified copy of the one in ModernGekko-Template.

**DolRecomp**, by ExpansionPak.
https://github.com/ExpansionPak/DolRecomp

The recompiler. It extracts the disc and translates PowerPC code to C. Also GPL-3.0.

**Dolphin**, the GameCube and Wii emulator, and ExpansionPak's RecompCore fork of it.
https://github.com/dolphin-emu/dolphin
https://github.com/ExpansionPak/RecompCore

ModernGekko vendors that fork as `vendor/dolphin`. The disc reading, the CPU core and much of the runtime in the resulting binary are Dolphin's code.

## Which licence applies

Dolphin's `COPYING` says most of its original source code is GPL-2.0-or-later, that per-file SPDX tags decide for any given file, and that in aggregate the tree is compatible with GPLv3. The vendored fork follows the same scheme. In the files this repo modifies:

- the five `Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore*` files carry `SPDX-License-Identifier: GPL-2.0-or-later`
- `GXRuntime/src/core/cpu.c` and `GXRuntime/include/core/cpu.h` carry `SPDX-License-Identifier: GPL-3.0-or-later`

Both are "or later", so either may be used under GPLv3, and that is what happens here. The combined work is distributed under GPL-3.0, whose full text is in `LICENSE`.

## What this repo changes

Sixteen files. Every one is a modified copy of an upstream file, none is new. Each carries a line saying it was modified by Colosseum Recomp on 2026-10-18.

Five from the ModernGekko and template trees, in commit 8b12429:

- `Makefile`
- `lib/ModernGekko/CMakeLists.txt`
- `lib/ModernGekko/tools/moderngekko_launcher.cpp`
- `lib/ModernGekko/tools/moderngekko_run.cpp`
- `lib/ModernGekko/tools/frontend_config.cpp`

Eleven from the vendored RecompCore tree, in commit dbb6dd0:

- `vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.cpp`
- `vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore.h`
- `vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Hooks.cpp`
- `vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_Run.cpp`
- `vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompCore_SMC.cpp`
- `vendor/dolphin/GXRuntime/include/core/cpu.h`
- `vendor/dolphin/GXRuntime/src/core/cpu.c`
- `vendor/dolphin/module-template/CMakeLists.txt`
- `vendor/dolphin/module-template/gen_module_tables.py`
- `vendor/dolphin/module-template/module_export.c`
- `vendor/dolphin/Data/Sys/GameSettings/GC6E01.ini`

`versions.txt` lists them again, against the commits they were taken from.

## The game settings file

`vendor/dolphin/Data/Sys/GameSettings/GC6E01.ini` is game specific patch metadata for the retail disc. It has OnFrame patch entries for the memory card save check, a data cache patch that is present but disabled and kept only as documentation, and ActionReplay codes for 16:9 widescreen and 60 fps. What it holds is memory addresses and patch values in a text file. No game code, no assets, and nothing that runs without the disc.

## A word in a vendored file

Line 10 of `GXRuntime/include/core/cpu.h` is the upstream comment `// more msvc shit fuck msvc`. That is verbatim text from the RecompCore source, written by its authors, not by this project. It is left as it is so the file stays diffable against upstream.

## Other libraries

Bundled by ModernGekko and Dolphin, not by this repo: Dear ImGui, SDL, libusb, mbedTLS, zlib-ng, LZO, liblzma, FatFs, MoltenVK and the Vulkan headers. Their licence files are in the upstream trees, under `vendor/dolphin/Externals` and `vendor/dolphin/LICENSES`, and their terms still apply to any binary you build from this source.

## The published archive

The download that carries the binaries also carries the paperwork. `licences/GPL-3.0.txt` is the licence text, `licences/LICENCES-INDEX.txt` says which document covers which file in the archive, and `licences/SOURCE-OFFER.txt` is the written offer of source, naming the pinned upstream commits the binaries were built from. The tools archive inside the download repeats that set in its own `licences/` folder, and the toolchain archive keeps the Python licence and a provenance note next to the interpreter.

## The game

Pokemon Colosseum is a game by Genius Sonority, published by Nintendo and The Pokemon Company. None of it is distributed here.

This repo contains no disc image, no extracted game files, no textures, no audio, no models and no recompiled game module. The one game specific file is the settings file described above. The `.gitignore` is written to keep the rest out, including the module the launcher builds, which is derived from Nintendo code.

To use this tool you need a disc you own and an image you made from it. Do not ask for images, and do not share the module you build.

## What this repo actually contains

- `overlay/`: sixteen modified files, fifteen source files and one game settings file, all listed in `versions.txt`
- `bootstrap.sh`, `bootstrap.bat`: fetch the pinned upstreams and apply the overlay
- `versions.txt`: the pinned commits and what the overlay replaces
- `README.md`, `LICENSE`, this file

Everything else is fetched or built, and is not redistributed here.
