# Colosseum Recomp

A Windows launcher that turns your own Pokemon Colosseum disc into a native build and runs it.

This repo does not contain the game. It contains the launcher source plus a small overlay that sits on top of three upstream projects. You supply the disc.

State: pre-release. The launcher builds and runs on my machine. The component download manifest still points at a loopback test host, so that download path only works here for now. See Components below.

## What it does

1. You pick a disc image.
2. It extracts the disc, reading the filesystem through Dolphin's DiscIO code.
3. It runs DolRecomp on the extracted main DOL and compiles the generated C into a module for that disc.
4. It loads the module in ModernGekko's runtime and starts the game.

The module is built from your image on your machine. It is cached against the DOL hash, so a second start with the same disc skips the build entirely.

## What you need

- A Pokemon Colosseum disc image made from a disc you own. `.iso` is the normal case. The picker also lists `.wbfs` and `.rvz` images found in your Documents folder.
- Windows 10 or 11, x64. The launcher uses WinHTTP and BCrypt for downloads and digest checks, so it is Windows only right now.
- A GameCube controller if you have one. A keyboard works too.
- To build: git, CMake 3.20 or newer, Ninja, and a C++20 compiler (MSVC 2022 or a recent MinGW GCC).
- A few GB of disk, mostly for the Dolphin tree and the module build.

## Get the source

    ./bootstrap.sh --with-dolphin

or from cmd:

    bootstrap.bat --with-dolphin

That clones three repos at pinned commits into `work/` and copies `overlay/` over them. The exact commits are in `versions.txt`. Without `--with-dolphin` it skips the Dolphin submodule, which is about 880 MB and which you need before you can build.

## Build

    cd work/ModernGekko-Template
    make tools

`make tools` builds the recompiler and the recompiled port. It does not build the launcher. The launcher and runner are separate CMake targets:

    cmake --build lib/ModernGekko/build --target moderngekko-launcher
    cmake --build lib/ModernGekko/build --target moderngekko-run

Both binaries land in `lib/ModernGekko/build`. Keep them in the same folder, because the launcher looks for `moderngekko-run` next to itself.

## Run

Start the launcher, pick your disc, and work through the two buttons: one extracts, one builds and then plays. The first build takes several minutes and the window shows the recompiler's progress as it goes. The finished module is written next to the launcher as `g<disc id>_recomp.dll`.

Two options exist for testing without the window:

    moderngekko-launcher.exe --extract path\to\disc.iso
    moderngekko-launcher.exe --fetch-components

State lives in `%LOCALAPPDATA%\moderngekko`:

    games\<disc id>\    the extracted disc
    Generated\          module build cache
    Logs\               build log, plus a dump of the build environment
    Runtime\            downloaded components

## Components

The launcher ships without a compiler. On first run it fetches what it needs from a manifest and checks every archive against a pinned SHA-256 before unpacking it. Once a component is in place it is never fetched again.

The manifest is `components.txt`, read from the folder the launcher sits in. One component per line, five fields:

    name version url sha256 probe

    name      directory under Runtime\
    version   part of the path, bumping it forces a re-fetch
    url       full URL to a zip
    sha256    lowercase hex digest of that zip
    probe     file inside the zip whose presence means the component is ready

Two components are used. `toolchain` is a GCC used to compile the module. `tools` carries `moderngekko-port.exe`, the recompiler driver.

As shipped, the manifest points at `http://127.0.0.1:8731`, a loopback test host I used while building the packaging step. It will fail on any other machine. That host has to be replaced before a release goes out. If there is no `components.txt` beside the launcher, it falls back to `moderngekko-port.exe` and `toolchain\bin` next to itself, which is what a development build does.

## Building without the launcher

The template Makefile still works on its own:

    make extract ISO=/path/to/disc.iso
    make recompile GAME=<slug>
    make run GAME=<slug>

The slug is the folder name under `extracted/`, taken from the ISO filename the first time you extract. `make help` lists the rest.

## Legal

The game is not distributed here in any form. No disc image, no extracted files, no recompiled module. You need your own copy of the disc, and you need to make the image yourself if you have not already.

The launcher links ModernGekko, which is GPL-3.0, so this project is GPL-3.0. See `LICENSE` and `NOTICE.md`.
