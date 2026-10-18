# Notice

Colosseum Recomp is a launcher built on other people's work. This file says whose.

## Upstream projects

**ModernGekko** and **ModernGekko-Template**, by ExpansionPak.
https://github.com/ExpansionPak/ModernGekko
https://github.com/ExpansionPak/ModernGekko-Template

These supply the recompiler runtime, the frontend framework, the per-game build pipeline, and most of the code this launcher sits next to. The launcher and runner in `overlay/` are modified copies of files from ModernGekko. The Makefile is a modified copy of the one in ModernGekko-Template.

**DolRecomp**, by ExpansionPak.
https://github.com/ExpansionPak/DolRecomp

The recompiler. It extracts the disc and translates PowerPC code to C.

**Dolphin**, the GameCube and Wii emulator.
https://github.com/dolphin-emu/dolphin

ModernGekko vendors a fork of Dolphin as `vendor/dolphin`. Dolphin is GPL-3.0. The disc reading, the CPU core and much of the runtime in the resulting binary are Dolphin's code, used under that licence.

Both ModernGekko and DolRecomp are GPL-3.0, so the combined work here is GPL-3.0 as well. The full text is in `LICENSE`.

## Other libraries

Bundled by ModernGekko and Dolphin, not by this repo:

- Dear ImGui, MIT
- SDL3, zlib
- libusb, LGPL-2.1
- mbedTLS, Apache-2.0
- zlib, zlib
- LZO, GPL-2.0 or later
- liblzma, public domain
- FatFs, BSD-1-Clause
- MoltenVK, Apache-2.0
- Vulkan-Headers, Apache-2.0

Their licence files ship with the upstream trees. Their terms still apply to any binary you build from this.

## The game

Pokemon Colosseum is a game by Genius Sonority, published by Nintendo and The Pokemon Company. None of it is distributed here.

This repo contains no disc image, no extracted game files, no textures, no audio, no models and no recompiled game module. The `.gitignore` is written to keep those out, including the module the launcher builds, which is derived from Nintendo code.

To use this tool you need a disc you own and an image you made from it. Do not ask for images, and do not share the module you build.

## What this repo actually contains

- `overlay/`: five modified source files, listed in `versions.txt`
- `bootstrap.sh`, `bootstrap.bat`: fetch the pinned upstreams and apply the overlay
- `versions.txt`: the pinned commits and what the overlay replaces
- `README.md`, `LICENSE`, this file

Everything else is fetched or built, and is not redistributed here.
