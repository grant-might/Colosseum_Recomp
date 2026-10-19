# Colosseum Recomp

A Windows launcher that turns your own Pokemon Colosseum disc into a native build and runs it.

This repo does not contain the game. It has the launcher source and the sixteen modified upstream files the launcher is built from. You supply the disc.

## What it does

1. You pick a disc image.
2. It extracts the disc, reading the filesystem through Dolphin's DiscIO code.
3. It runs DolRecomp on the extracted main DOL and compiles the generated C into a module for that disc.
4. It loads the module in ModernGekko's runtime and starts the game.

The module is built from your image on your machine. It is cached against the DOL hash, so a second start with the same disc skips the build entirely.

## What you download

One archive, `colosseum-launcher-1.zip`, 207.4 MiB. Everything the launcher needs to build is inside it, and none of it touches the network.

| path inside the archive | what it is |
|---|---|
| `ColosseumRecomp.exe` | the launcher |
| `ColosseumRecompRun.exe` | the runner it starts |
| `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` | the MSVC runtime the two exes import, deployed app-local |
| `components.txt` | what the two component archives are, with the digest of each |
| `colosseum-toolchain-16.2.0-ucrt-r3.zip` | GCC 16.2.0 (winlibs, UCRT, MinGW-w64 14.0.0), CMake 4.4.3, Ninja 1.13.2, CPython 3.13.16 |
| `colosseum-tools-2.zip` | `moderngekko-port.exe`, `dolrecomp.exe`, the module template, GXRuntime, the StaticRecomp ABI headers |
| `licences/` | the GPL-3.0 text, an index of what covers what, and a written offer of source |
| `THIRD-PARTY-NOTICES.txt` | the MSVC runtime notice |

Unzip it where you want the install to live, then run `ColosseumRecomp.exe` from that folder. On first run the launcher unpacks the two component archives into `%LOCALAPPDATA%\ColosseumRecomp\Runtime`, checking each against the SHA-256 pinned in `components.txt` before it unpacks anything. That is the whole setup. There is no download step and no first-run network access.

The archives stay where they are. They are read from the install folder, verified, unpacked, and left alone, so the same folder can set up a second user directory or be copied to another machine. If one of them is missing, the launcher names the file it wanted and stops.

Sizes, measured from the shipped archives: the toolchain is 178.9 MiB zipped and 655.7 MiB unpacked, the tools archive is 17.5 MiB zipped and 42.3 MiB unpacked.

## What you need

- A Pokemon Colosseum disc image made from a disc you own. `.iso` is the normal case. The picker also lists `.wbfs` and `.rvz` images it finds under your Documents folder.
- Windows 10 or 11, x64. The launcher checks digests with BCrypt and the bundled toolchain targets UCRT, so it is Windows only right now.
- A GameCube controller if you have one. A keyboard works too.
- About 700 MB for the unpacked components, plus room for the module build.
- To build from source: git, CMake 3.20 or newer, Ninja, and a C++20 compiler (MSVC 2022 or a recent MinGW GCC).
- A few GB of disk if you are building, mostly for the Dolphin tree.

## Run

Start `ColosseumRecomp.exe`, pick your disc, and work through the two buttons: one extracts, one builds and then plays. The first build takes several minutes and the window shows the recompiler's progress as it goes. The finished module is written next to the launcher as `g<disc id>_recomp.dll`, and the launcher starts `ColosseumRecompRun.exe` from that same folder.

Two options exist for testing without the window:

    ColosseumRecomp.exe --extract path\to\disc.iso
    ColosseumRecomp.exe --fetch-components

`--fetch-components` does the unpack and digest check for the archives beside the launcher and exits. It is the same work the first launch does, so it is a way to prove an install before handing it to someone.

State lives in `%LOCALAPPDATA%\ColosseumRecomp`:

    config.ini          resolution and window settings
    games\<disc id>\    the extracted disc
    Generated\          module build cache
    Logs\               build log, a dump of the build environment, the launcher log
    Runtime\            the unpacked components, one folder per name and version

## Components

The launcher ships without a compiler, so the compiler and the recompiler travel with it. `components.txt` sits beside the launcher and says what the archives are. One component per line, five fields:

    name version archive sha256 probe

    name      directory under Runtime\
    version   part of the path, bumping it forces a re-unpack
    archive   file name of the zip that ships beside the launcher
    sha256    lowercase hex digest of that zip
    probe     file inside the zip whose presence means the component is ready

Two components are used. `toolchain` is the GCC that compiles the module. `tools` carries `moderngekko-port.exe`, the recompiler driver.

The third field is a bare file name in the shipped bundle. The parser still accepts an `http(s)://` URL there and will download it if one is given, which is there for a hosted copy of the same archives. The bundle does not use that path and needs no network.

## Build from source

    ./bootstrap.sh --with-dolphin

or from cmd:

    bootstrap.bat --with-dolphin

That clones three repos at pinned commits into `work/` and copies `overlay/` over them. The exact commits are in `versions.txt`. Without `--with-dolphin` it skips the Dolphin submodule, which is about 880 MB and which you need before you can build.

    cd work/ModernGekko-Template
    make tools

`make tools` builds the recompiler and the recompiled port. It does not build the launcher or the runner, and on its own it does not give you the published product, because the stock names come from ModernGekko. Configure with the branding set explicitly:

    cmake -S lib/ModernGekko -B lib/ModernGekko/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DMODERNGEKKO_FRONTEND_NAME="Colosseum Recomp" \
      -DMODERNGEKKO_LAUNCHER_OUTPUT_NAME=ColosseumRecomp \
      -DMODERNGEKKO_RUNNER_OUTPUT_NAME=ColosseumRecompRun \
      -DMODERNGEKKO_USER_DIRECTORY_NAME=ColosseumRecomp \
      -DMODERNGEKKO_LOG_FILENAME=ColosseumRecomp.log \
      -DMODERNGEKKO_REQUIRED_DISC_ID=GC6E01 \
      -DMODERNGEKKO_GAMECUBE_CONTROLLERS=ON

    cmake --build lib/ModernGekko/build --target moderngekko-launcher
    cmake --build lib/ModernGekko/build --target moderngekko-run

Those are the values from the CMake cache of the shipped build, quoted as they are stored there:

    CMAKE_BUILD_TYPE:STRING=Release
    MODERNGEKKO_FRONTEND_NAME:STRING=Colosseum Recomp
    MODERNGEKKO_LAUNCHER_OUTPUT_NAME:STRING=ColosseumRecomp
    MODERNGEKKO_RUNNER_OUTPUT_NAME:STRING=ColosseumRecompRun
    MODERNGEKKO_USER_DIRECTORY_NAME:STRING=ColosseumRecomp
    MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS:BOOL=OFF
    MODERNGEKKO_LOG_FILENAME:STRING=ColosseumRecomp.log
    MODERNGEKKO_REQUIRED_DISC_ID:STRING=GC6E01
    MODERNGEKKO_GAMECUBE_CONTROLLERS:BOOL=ON

The CMake target names do not change, only the file names they produce. Both binaries land in `lib/ModernGekko/build` as `ColosseumRecomp.exe` and `ColosseumRecompRun.exe`. Keep them in the same folder, because the launcher starts the runner from beside itself. The module it builds is published beside them too, and the runner looks there first.

Leave the flags out and you get stock ModernGekko: `ModernGekko.exe` and `moderngekko-run.exe`, state in `%LOCALAPPDATA%\moderngekko`, and no disc id restriction, so the picker will accept any GameCube image it can read.

## Building without the launcher

The template Makefile still works on its own:

    make extract ISO=/path/to/disc.iso
    make recompile GAME=<slug>
    make run GAME=<slug>

The slug is the folder name under `extracted/`, taken from the ISO filename the first time you extract. `make help` lists the rest.

## Legal

The game is not distributed here in any form. No disc image, no extracted files, no recompiled module. You need your own copy of the disc, and you need to make the image yourself if you have not already.

The launcher links ModernGekko, which is GPL-3.0, so this project is GPL-3.0. See `LICENSE` and `NOTICE.md`. `NOTICE.md` also lists the sixteen upstream files this repo modifies and the licences those files came under.
