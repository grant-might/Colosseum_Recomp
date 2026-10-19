// Modified by Colosseum Recomp on 2026-10-18, see NOTICE.md.
#include "frontend_config.hpp"
#include "dol_patch.hpp"
#include "launcher_savestates.hpp"
#include "moderngekko/game.hpp"
#include "netplay_session.hpp"

#include "DiscIO/DiscExtractor.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#if defined(_WIN32)
// For first-run component acquisition: WinHTTP to fetch, BCrypt to verify. Both are in-box, so the
// launcher gains a download path without gaining a third-party dependency. The macros are guarded
// because the target already defines them and redefining is a C4005 warning.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// Display brand shown in the launcher's window title and heading. Plain ASCII on purpose: no
// encoding hazard anywhere in the build chain, and it avoids using a trademarked name in a
// product title. Filenames and the user directory reuse the same spelling.
#ifndef MG_DISPLAY_BRAND
#define MG_DISPLAY_BRAND "Colosseum Recomp"
#endif

#ifndef MODERNGEKKO_FRONTEND_NAME
#define MODERNGEKKO_FRONTEND_NAME "ModernGekko"
#endif

#ifndef MODERNGEKKO_RUNNER_FILENAME
#define MODERNGEKKO_RUNNER_FILENAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

#ifndef MODERNGEKKO_LOG_FILENAME
#define MODERNGEKKO_LOG_FILENAME "ModernGekko.log"
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
constexpr std::string_view CONTROLLER_PROFILE_NAME = "GCPadNew.ini";
constexpr std::string_view GENERATED_PROFILE_LABEL = "GameCube profile";
#else
constexpr std::string_view CONTROLLER_PROFILE_NAME = "WiimoteNew.ini";
constexpr std::string_view GENERATED_PROFILE_LABEL = "sideways profile";
#endif

struct ExtractionState
{
  std::atomic<bool> running{false};
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> total{1};
  std::mutex mutex;
  std::string status;
  std::string error;
  std::optional<fs::path> finished_game;
};

// A module build outlives a frame by minutes, so its progress lives here and the UI polls it.
// `log_tail` carries the recompiler's own output so a failure is diagnosable from the launcher
// instead of from a console the user never saw.
struct BuildState
{
  std::atomic<bool> running{false};
  std::atomic<unsigned> completed{0};
  std::atomic<unsigned> total{1};
  std::mutex mutex;
  std::string status;
  int stage_rank{0};         // latest phase, shown as a label only; the bar is bytes, not phases
  std::string build_root;    // the directory being measured, guarded by mutex
  std::atomic<unsigned long long> bytes_accounted{0}; // bytes ever written; monotonic by construction
  std::atomic<unsigned long long> bytes_expected{0};  // what a complete build of this disc weighs
  std::string error;
  std::string log_tail;
  std::atomic<unsigned long long> started_ms{0};
  std::atomic<bool> attempted{false}; // one attempt per request; see the trigger for why
  std::optional<fs::path> finished_module;
};

struct DialogState
{
  std::mutex mutex;
  std::optional<fs::path> selected;
  std::string error;
};

struct ControllerOption
{
  std::string label;
  std::string device;
};

std::string RequiredGameError(const moderngekko::GameMetadata& metadata)
{
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (metadata.disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return "expected disc ID " MODERNGEKKO_REQUIRED_DISC_ID ", found " + metadata.disc_id;
#endif
#ifdef MODERNGEKKO_REQUIRED_DOL_SHA256
  if (metadata.dol_sha256 != MODERNGEKKO_REQUIRED_DOL_SHA256)
    return "main DOL does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_REL_SHA256
  if (metadata.rel_sha256 != MODERNGEKKO_REQUIRED_REL_SHA256)
    return "_Main.rel does not match the pinned release";
#endif
#ifdef MODERNGEKKO_REQUIRED_ASSETS_SHA256
  if (metadata.assets_sha256 != MODERNGEKKO_REQUIRED_ASSETS_SHA256)
    return "game assets do not match the pinned release";
#endif
  return {};
}

std::vector<ControllerOption> EnumerateControllers()
{
  std::vector<ControllerOption> result;
  std::unordered_map<std::string, int> device_ids;
  int count = 0;
  SDL_JoystickID* joystick_ids = SDL_GetJoysticks(&count);
  for (int i = 0; i < count; ++i)
  {
    const SDL_JoystickID joystick_id = joystick_ids[i];
    const bool gamepad = SDL_IsGamepad(joystick_id);
    const char* name_value =
        gamepad ? SDL_GetGamepadNameForID(joystick_id) : SDL_GetJoystickNameForID(joystick_id);
    const std::string name = name_value && *name_value ? name_value : "Unknown Controller";
    const int device_id = device_ids[name]++;
    if (!gamepad)
      continue;
    ControllerOption option;
    option.label = device_id == 0 ? name : name + " (" + std::to_string(device_id + 1) + ")";
    option.device = "SDL/" + std::to_string(device_id) + "/" + name;
    result.emplace_back(std::move(option));
  }
  SDL_free(joystick_ids);
  return result;
}

int FindController(const std::vector<ControllerOption>& controllers, std::string_view device)
{
  const auto found = std::ranges::find(controllers, device, &ControllerOption::device);
  return found == controllers.end() ? -1 : static_cast<int>(found - controllers.begin());
}

fs::path DefaultUserDirectory()
{
#ifdef MODERNGEKKO_USER_DIRECTORY_IN_DOCUMENTS
#if defined(_WIN32)
  if (const char* user_profile = std::getenv("USERPROFILE"))
    return fs::path(user_profile) / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Documents" / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA"))
    return fs::path(local_app_data) / MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char* xdg = std::getenv("XDG_DATA_HOME"))
    return fs::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".local/share" / MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

fs::path DocumentsDirectory()
{
#if defined(_WIN32)
  if (const char* user_profile = std::getenv("USERPROFILE"))
    return fs::path(user_profile) / "Documents";
#endif
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Documents";
  return fs::current_path();
}

fs::path ExecutableDirectory(const char* argv0)
{
  std::error_code ec;
#if defined(__linux__)
  const fs::path proc_executable = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const fs::path executable = fs::weakly_canonical(argv0, ec);
  return ec ? fs::current_path() : executable.parent_path();
}

fs::path SiblingExecutable(const fs::path& release_directory, std::string name)
{
#if defined(_WIN32)
  name += ".exe";
#endif
  const fs::path sibling = release_directory / name;
  return fs::is_regular_file(sibling) ? sibling : fs::path(name);
}

bool ApplyBundledDolPatch(const fs::path& game_root, const fs::path& release_directory,
                          bool* changed, std::string* error)
{
#ifdef MODERNGEKKO_DOL_PATCH_MANIFEST
  if (game_root.empty() || !fs::is_directory(game_root))
  {
    *changed = false;
    return true;
  }
  return moderngekko::frontend::ApplyDolPatchManifest(
      game_root / "sys" / "main.dol", release_directory / MODERNGEKKO_DOL_PATCH_MANIFEST,
      changed, error);
#else
  *changed = false;
  return true;
#endif
}

fs::path DefaultGameFile(const fs::path& user_directory, const fs::path& release_directory)
{
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  return release_directory / "default-game.txt";
#else
  return user_directory / "default-game.txt";
#endif
}

fs::path ReadDefaultGame(const fs::path& user_directory, const fs::path& release_directory)
{
  std::ifstream file(DefaultGameFile(user_directory, release_directory));
  std::string value;
  std::getline(file, value);
  if (!value.empty() && value.back() == '\r')
    value.pop_back();
  fs::path game(value);
  if (game.is_relative())
    game = release_directory / game;
  return game;
}

bool WriteDefaultGame(const fs::path& user_directory, const fs::path& release_directory,
                      const fs::path& game, std::string* error)
{
  const fs::path destination = DefaultGameFile(user_directory, release_directory);
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  std::ofstream file(destination, std::ios::trunc);
  if (!file)
  {
    if (error)
      *error = "can't save default-game.txt";
    return false;
  }
  fs::path stored = game;
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path relative = fs::relative(game, release_directory, ec);
  if (!ec && !relative.empty())
    stored = relative;
#endif
  file << stored.string() << '\n';
  return true;
}

std::vector<fs::path> FindDiscImages()
{
  std::vector<fs::path> images;
  std::error_code ec;
  const fs::path documents = DocumentsDirectory();
  if (!fs::is_directory(documents, ec))
    return images;
  fs::recursive_directory_iterator iterator(documents,
                                            fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (iterator != end)
  {
    if (iterator.depth() > 4)
      iterator.disable_recursion_pending();
    if (iterator->is_regular_file(ec))
    {
      std::string extension = iterator->path().extension().string();
      std::ranges::transform(extension, extension.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (extension == ".wbfs" || extension == ".iso" || extension == ".rvz")
        images.push_back(iterator->path());
    }
    iterator.increment(ec);
    if (ec)
      ec.clear();
  }
  std::ranges::sort(images);
  return images;
}

// --- first-run component acquisition ---------------------------------------------------------
//
// The launcher ships without a compiler. A user who already has one should not download ~400 MB of
// toolchain, and one who does not cannot build anything without it, so each component is fetched
// once, verified against a pinned digest, extracted and left in place. Presence of the probe file
// is the test, so a second run does nothing at all.
//
// Specs live in components.txt beside the launcher rather than in this file: no URL or digest is
// invented here, and packaging can change them without a rebuild.

struct ComponentSpec
{
  std::string name;     // directory under <user dir>/Runtime
  std::string version;  // part of the path; bumping it forces a re-fetch
  std::string archive;  // archive beside the launcher; a URL is still accepted and downloaded
  std::string sha256;   // lowercase hex of the archive
  std::string probe;    // file whose presence means the component is usable
};

struct SetupState
{
  std::atomic<bool> running{false};
  std::atomic<bool> complete{false};
  std::atomic<unsigned long long> received{0};
  std::atomic<unsigned long long> total{0};
  std::mutex mutex;
  std::string phase;
  std::string error;
};

std::vector<ComponentSpec> ReadComponentSpecs(const fs::path& release_directory)
{
  std::vector<ComponentSpec> specs;
  std::ifstream file(release_directory / "components.txt");
  if (!file)
    return specs;
  std::string line;
  while (std::getline(file, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#')
      continue;
    std::vector<std::string> fields;
    std::size_t begin = first;
    while (begin < line.size())
    {
      const std::size_t space = line.find_first_of(" \t", begin);
      fields.push_back(line.substr(begin, space - begin));
      if (space == std::string::npos)
        break;
      begin = line.find_first_not_of(" \t", space);
      if (begin == std::string::npos)
        break;
    }
    if (fields.size() != 5)
      continue;
    specs.push_back(ComponentSpec{fields[0], fields[1], fields[2], fields[3], fields[4]});
  }
  return specs;
}

#if defined(_WIN32)
std::wstring Widen(const std::string& text)
{
  if (text.empty())
    return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0)
    return {};
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

// Fetch to a file, reporting progress. GitHub release URLs redirect to a CDN, and WinHTTP refuses
// redirects unless it is told otherwise, so the policy has to be set explicitly.
bool DownloadFile(const std::string& url, const fs::path& destination, SetupState* state)
{
  const std::wstring wide_url = Widen(url);
  if (wide_url.empty())
  {
    std::lock_guard lock(state->mutex);
    state->error = "component URL is not valid UTF-8";
    return false;
  }

  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{};
  wchar_t path[4096]{};
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts))
  {
    std::lock_guard lock(state->mutex);
    state->error = "component URL could not be parsed";
    return false;
  }

  HINTERNET session = WinHttpOpen(L"ColosseumRecomp/1.0",
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connection =
      session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
  const DWORD request_flags =
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request =
      connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, request_flags)
                 : nullptr;

  auto finish = [&](std::string message)
  {
    if (request)
      WinHttpCloseHandle(request);
    if (connection)
      WinHttpCloseHandle(connection);
    if (session)
      WinHttpCloseHandle(session);
    if (!message.empty())
    {
      std::lock_guard lock(state->mutex);
      state->error = std::move(message);
    }
    return message.empty();
  };

  if (!session || !connection || !request)
    return finish("could not reach the download host");
  {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  }
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr))
    return finish("download request failed");

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200)
    return finish("download host returned HTTP " + std::to_string(status));

  DWORD content_length = 0;
  DWORD length_size = sizeof(content_length);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_size,
                          WINHTTP_NO_HEADER_INDEX))
    state->total = content_length;

  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out)
    return finish("could not open the download location");

  std::vector<char> buffer(1 << 16);
  for (;;)
  {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available))
      return finish("the download stalled");
    if (available == 0)
      break;
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
      return finish("the download was interrupted");
    if (read == 0)
      break;
    out.write(buffer.data(), read);
    state->received = state->received.load() + read;
  }
  out.close();
  if (!out)
    return finish("could not write the downloaded archive");
  return finish({});
}

// Verify with the in-box hashing provider: an unverified download that gets executed is the whole
// risk of this path, so the digest is checked before anything is extracted.
std::string Sha256Hex(const fs::path& file)
{
  // BCrypt returns NTSTATUS, where zero means success - the opposite convention to the Win32 BOOL
  // APIs. Writing these guards as `if (!call)` makes every call look like a failure and the
  // function returns empty for every input, so the comparison is spelled out.
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    return {};
  DWORD object_size = 0;
  DWORD result_size = 0;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size,
                        0) != 0)
  {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
  }
  std::vector<unsigned char> object(object_size);
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) != 0)
  {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
  }
  std::ifstream in(file, std::ios::binary);
  std::vector<char> buffer(1 << 16);
  bool ok = static_cast<bool>(in);
  while (ok && in)
  {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                       static_cast<ULONG>(got), 0))
      ok = false;
  }
  unsigned char digest[32]{};
  if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0))
    ok = false;
  if (ok)
  {
    static const char* digits = "0123456789abcdef";
    hex.reserve(64);
    for (unsigned char byte : digest)
    {
      hex.push_back(digits[byte >> 4]);
      hex.push_back(digits[byte & 0x0f]);
    }
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return hex;
}

// Windows ships bsdtar as C:\Windows\System32\tar.exe, which reads zip. In-box, no dependency.
bool ExtractArchive(const fs::path& archive, const fs::path& destination, SetupState* state)
{
  const char* system_root = std::getenv("SystemRoot");
  const fs::path tar =
      fs::path(system_root ? system_root : "C:\\Windows") / "System32" / "tar.exe";
  if (!fs::is_regular_file(tar))
  {
    std::lock_guard lock(state->mutex);
    state->error = "the system archiver is missing: " + tar.string();
    return false;
  }
  std::error_code ec;
  fs::create_directories(destination, ec);
  if (ec)
  {
    std::lock_guard lock(state->mutex);
    state->error = "could not create the component directory: " + ec.message();
    return false;
  }
  const std::vector<std::string> storage = {tar.string(), "-xf", archive.string(), "-C",
                                            destination.string()};
  std::vector<const char*> arguments;
  arguments.reserve(storage.size() + 1);
  for (const std::string& argument : storage)
    arguments.push_back(argument.c_str());
  arguments.push_back(nullptr);
  SDL_Process* process = SDL_CreateProcess(arguments.data(), false);
  if (!process)
  {
    std::lock_guard lock(state->mutex);
    state->error = "could not start the archiver: " + std::string(SDL_GetError());
    return false;
  }
  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  if (!waited || exit_code != 0)
  {
    std::lock_guard lock(state->mutex);
    state->error = "the archiver failed with exit code " + std::to_string(exit_code);
    return false;
  }
  return true;
}
#endif

// Install anything that is missing. Components already present are skipped, which is what makes this
// safe to call on every launch. A shipped install carries the component archives beside the launcher,
// so an archive found there is unpacked in place and nothing is fetched.
bool EnsureComponents(const fs::path& user_directory, const fs::path& release_directory,
                      SetupState* state)
{
  const std::vector<ComponentSpec> specs = ReadComponentSpecs(release_directory);
  if (specs.empty())
  {
    std::lock_guard lock(state->mutex);
    state->complete = true;
    return true;
  }
  const fs::path runtime = user_directory / "Runtime";
  for (const ComponentSpec& spec : specs)
  {
    const fs::path root = runtime / spec.name / spec.version;
    std::error_code ec;
    if (fs::is_regular_file(root / spec.probe, ec))
      continue;

    // Where the bytes come from is decided by what is beside the launcher, not by a URL. A bundled
    // archive is read where it lies and must survive this call: it belongs to the install, and the
    // next user directory needs the same file, so it is never deleted the way a temp copy is.
    const fs::path bundled = release_directory / spec.archive;
    const bool from_bundle = fs::is_regular_file(bundled, ec);
    const bool hosted = spec.archive.find("://") != std::string::npos;
    if (!from_bundle && !hosted)
    {
      std::lock_guard lock(state->mutex);
      state->error = "the " + spec.name + " archive " + spec.archive +
                     " is missing beside the launcher - reinstall from the original download";
      return false;
    }
    // Downloads land in the user directory, never in the install directory.
    const fs::path download = runtime / (spec.name + "-" + spec.version + ".zip");
    const fs::path archive = from_bundle ? bundled : download;

    {
      std::lock_guard lock(state->mutex);
      state->phase = (from_bundle ? "Unpacking " : "Downloading ") + spec.name + " " + spec.version;
    }
    state->received = 0;
    state->total = 0;
#if !defined(_WIN32)
    {
      std::lock_guard lock(state->mutex);
      state->error = "component download is only implemented on Windows";
      return false;
    }
#else
    if (!from_bundle && !DownloadFile(spec.archive, download, state))
      return false;

    {
      std::lock_guard lock(state->mutex);
      state->phase = "Verifying " + spec.name;
    }
    // Always the file that is about to be extracted: the bundled archive's digest is pinned too.
    const std::string digest = Sha256Hex(archive);
    if (digest.empty())
    {
      std::lock_guard lock(state->mutex);
      state->error = "could not compute the digest of the " + spec.name + " archive";
      return false;
    }
    if (digest != spec.sha256)
    {
      std::lock_guard lock(state->mutex);
      state->error = "the " + spec.name +
                     " archive did not match its pinned digest - refusing to use it";
      if (!from_bundle)
        fs::remove(download, ec);
      return false;
    }

    {
      std::lock_guard lock(state->mutex);
      state->phase = "Unpacking " + spec.name;
    }
    if (!ExtractArchive(archive, root, state))
      return false;
    if (!fs::is_regular_file(root / spec.probe, ec))
    {
      std::lock_guard lock(state->mutex);
      state->error = "the " + spec.name + " archive did not contain " + spec.probe;
      return false;
    }
    if (!from_bundle)
      fs::remove(download, ec);
#endif
  }
  {
    std::lock_guard lock(state->mutex);
    state->phase = "Ready to recompile";
    state->complete = true;
  }
  return true;
}

// --- recompiled module build ---------------------------------------------------------------

constexpr std::string_view MODULE_EXTENSION = ".dll";

std::string ModuleFileName(const std::string& disc_id)
{
  return "g" + disc_id + "_recomp" + std::string(MODULE_EXTENSION);
}

// The runner looks beside itself first and only then in the user directory (see
// moderngekko_run.cpp). Publishing beside the launcher therefore keeps one install self-contained
// and removes the chance of a stale module in the user directory silently taking precedence.
fs::path PublishedModulePath(const fs::path& release_directory, const std::string& disc_id)
{
  return release_directory / ModuleFileName(disc_id);
}

// The recompiler keys its artifact cache on the game's DOL hash and the toolchain, so a manifest
// whose dol_sha256 matches is by construction a build of this exact image with these exact tools.
// That is what makes a rebuild skippable rather than merely probably-unnecessary.
std::optional<fs::path> FindCurrentModule(const fs::path& cache_root, const std::string& disc_id,
                                          const std::string& dol_sha256)
{
  std::error_code ec;
  const fs::path per_game = cache_root / disc_id;
  if (!fs::is_directory(per_game, ec))
    return std::nullopt;
  for (const fs::directory_entry& entry : fs::directory_iterator(per_game, ec))
  {
    if (ec)
      break;
    if (!entry.is_directory(ec))
      continue;
    std::ifstream manifest(entry.path() / "manifest.txt");
    if (!manifest)
      continue;
    std::string line;
    while (std::getline(manifest, line))
    {
      if (line.back() == '\r')
        line.pop_back();
      if (line != "dol_sha256=" + dol_sha256)
        continue;
      const fs::path module = entry.path() / ModuleFileName(disc_id);
      if (fs::is_regular_file(module, ec))
        return module;
      break;
    }
  }
  return std::nullopt;
}

// The recompiler runs its configure step through a shell, so the child's PATH decides which shell
// that is. If an MSYS/POSIX sh.exe is first on PATH it rewrites the Windows paths it is given and
// the module template argument comes out as "/home/.../build/C:/...", which fails cmake before
// ninja ever runs - the build then sits at 0% with no useful error. So the bundled toolchain goes
// first and any inherited entry that would supply a POSIX shell is dropped.
std::string BuildSearchPath(const fs::path& toolchain_bin)
{
  std::vector<std::string> entries;
  if (fs::is_directory(toolchain_bin))
    entries.push_back(toolchain_bin.string());
  const char* system_root = std::getenv("SystemRoot");
  const fs::path windows = system_root ? system_root : "C:\\Windows";
  entries.push_back((windows / "System32").string());
  entries.push_back(windows.string());
  // Unconditional system baseline. On the operator's machine SDL's environment lookup returned
  // nothing for PATH - the launcher's own dump recorded "PATH=<unset>" - which left the child with a
  // three-entry PATH and made configure choose a different generator than the identical command run
  // from a shell. These four directories are what a Windows child needs to behave normally.
  entries.push_back((windows / "System32" / "Wbem").string());
  entries.push_back((windows / "System32" / "WindowsPowerShell" / "v1.0").string());

  // std::getenv is case-insensitive on Windows, so this finds "Path" (the usual spelling in the
  // registry) as well as "PATH"; SDL's lookup was not finding either.
  const char* existing = std::getenv("PATH");
  if (existing == nullptr)
    existing = std::getenv("Path");
  if (existing != nullptr && existing[0] != '\0')
  {
    const std::string inherited = existing;
    std::size_t begin = 0;
    while (begin <= inherited.size())
    {
      const std::size_t separator = inherited.find(';', begin);
      const std::string entry =
          inherited.substr(begin, separator == std::string::npos ? std::string::npos
                                                                : separator - begin);
      if (!entry.empty())
      {
        std::error_code ec;
        const fs::path directory(entry);
        // Anything that could supply a POSIX shell or a competing cmake is dropped. The shell is what
        // rewrites Windows paths into "/home/.../build/C:/..." and kills configure; the competing cmake
        // is how devkitPro's MSYS build wins despite the bundled toolchain being listed first.
        // fs::exists, not fs::is_regular_file: the App Execution Aliases in WindowsApps - which include
        // bash.exe - are reparse points, for which is_regular_file reports false, so a real bash
        // survived the filter and was reachable by the child.
        const bool supplies_posix_shell = fs::exists(directory / "sh.exe", ec) ||
                                          fs::exists(directory / "bash.exe", ec);
        const bool supplies_competing_cmake = fs::exists(directory / "cmake.exe", ec);
        if (!supplies_posix_shell && !supplies_competing_cmake)
          entries.push_back(entry);
      }
      if (separator == std::string::npos)
        break;
      begin = separator + 1;
    }
  }

  std::string combined;
  for (const std::string& entry : entries)
  {
    if (!combined.empty())
      combined += ';';
    combined += entry;
  }
  return combined;
}

// Rank the recompiler's phases, because line order is not progress order: the port prints each
// step's banner when that step finishes, so "applied 2 default DOL patches" arrives *after* the
// 4890 translation lines. Tracking the highest rank seen rather than the last one is what stops
// the panel appearing to move backwards. Each stage restarts its own counter, hence the step label.
const char* const kStageNames[] = {
    "",                             // 0: nothing observed yet
    "Preparing the game files",     // 1
    "Translating PowerPC code",     // 2: one dolrecomp pass, 4890 chunks
    "Configuring the module build", // 3: one cmake -E env configure
    "Building the module",          // 4: one cmake --build: tables, objects, link
    "Finishing up",                 // 5: publish the module
};
constexpr int kStageCount = 5;

// Progress is bytes the build has *written*, not bytes currently on disk. The build deletes its
// intermediates between phases - dolrecomp's objects get consumed and pruned, CMake cleans up - so a
// resident total rises and falls and the bar runs backwards. Each path therefore contributes its
// largest observed size, once, which can only ever increase.
std::uintmax_t AccountBuildBytes(const fs::path& root, std::map<std::string, std::uintmax_t>& seen)
{
  std::error_code ignored;
  std::uintmax_t total = 0;
  for (auto& entry : seen)
    total += entry.second;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ignored),
       end;
       it != end; it.increment(ignored))
  {
    if (!it->is_regular_file(ignored))
      continue;
    const std::uintmax_t size = it->file_size(ignored);
    std::uintmax_t& high_water = seen[it->path().string()];
    if (size > high_water)
      total += size - high_water; // count the growth, never the shrinkage
    high_water = std::max(high_water, size);
  }
  return total;
}

// The expected total is learned rather than guessed: a completed build's size is recorded and reused,
// because a fixed toolchain and backend produce the same artefacts for the same disc. It lives outside
// the build cache so that clearing the cache does not throw the calibration away.
constexpr unsigned long long kDefaultExpectedBuildBytes = 1200ull * 1024ull * 1024ull;

fs::path ExpectedBuildBytesPath(const fs::path& user_directory)
{
  return user_directory / "Logs" / "build-size-bytes.txt";
}

unsigned long long ReadExpectedBuildBytes(const fs::path& user_directory)
{
  std::ifstream in(ExpectedBuildBytesPath(user_directory));
  unsigned long long value = 0;
  if (in >> value && value > 0)
    return value;
  return kDefaultExpectedBuildBytes; // until one build has been measured end to end
}

// Keep the previous attempt's log before truncating this one. It is opened with "w", so a retry
// would otherwise erase the error that explains why the retry is happening - the one case where the
// log matters most. One rotation slot is enough: the newest failure is the one worth reading.
void RotateBuildLog(const fs::path& log_path)
{
  std::error_code ignored;
  if (!fs::exists(log_path, ignored) || fs::file_size(log_path, ignored) == 0)
    return;
  fs::rename(log_path, log_path.parent_path() / "ModuleBuild.previous.log", ignored);
}

int BuildStageRankFromLine(const std::string& line)
{
  static const struct
  {
    const char* marker;
    int rank;
  } kMarkers[] = {
      {"applied", 1},
      {"applying", 1},
      {"Emitting", 2},
      {"Build files have been written", 3},
      {"Configuring done", 3},
      {"Generating", 4},
      {"Building C object", 4},
      {"Linking", 4},
      {"built module", 5},
  };
  int rank = 0;
  for (const auto& marker : kMarkers)
  {
    if (marker.rank > rank && line.find(marker.marker) != std::string::npos)
      rank = marker.rank;
  }
  return rank;
}

// Keep CMake's own diagnostics out of the failed build's cache before it is cleared. The removal
// takes the CMakeCache and CMakeError log with it, and those are what explain the failure - the
// cleanup destroyed the evidence three times in one evening. Only these names are copied, so a
// failure cannot litter Logs/ with build intermediates; a missing cache is simply a no-op.
void PreserveBuildDiagnostics(const fs::path& cache_root, const fs::path& user_directory)
{
  std::error_code copy_error;
  for (const auto& entry : fs::recursive_directory_iterator(
           cache_root, fs::directory_options::skip_permission_denied, copy_error))
  {
    const std::string name = entry.path().filename().string();
    if (name == "CMakeCache.txt" || name == "CMakeError.log" || name == "CMakeOutput.log")
      fs::copy_file(entry.path(), user_directory / "Logs" / name,
                    fs::copy_options::overwrite_existing, copy_error);
  }
}

// Poll the build log: keeps the tail for display and derives progress from ninja's "[12/345]" lines.
void RefreshBuildLog(BuildState* state, const fs::path& log_path)
{
  std::ifstream log(log_path, std::ios::binary);
  if (!log)
    return;
  std::string tail;
  std::string line;
  int stage_rank = 0;
  unsigned completed = 0;
  unsigned total = 0;
  while (std::getline(log, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    tail += line;
    tail += '\n';
    if (tail.size() > 4000)
      tail.erase(0, tail.size() - 4000);
    const int rank = BuildStageRankFromLine(line);
    if (rank > stage_rank)
      stage_rank = rank;
    if (line.front() != '[')
      continue;
    const std::size_t slash = line.find('/');
    const std::size_t close = line.find(']');
    if (slash == std::string::npos || close == std::string::npos || slash >= close)
      continue;
    const unsigned done = static_cast<unsigned>(std::strtoul(line.c_str() + 1, nullptr, 10));
    const unsigned all =
        static_cast<unsigned>(std::strtoul(line.c_str() + slash + 1, nullptr, 10));
    if (all > 0)
    {
      completed = done;
      total = all;
    }
  }
  std::lock_guard lock(state->mutex);
  state->log_tail = std::move(tail);
  if (stage_rank > state->stage_rank)
    state->stage_rank = stage_rank;
  if (total > 0)
  {
    state->completed = completed;
    state->total = total;
  }
}

// Where the recompiler and its toolchain live: the fetched components if present, otherwise
// beside the launcher, which keeps a development checkout working without a bundle.
fs::path ResolvePortExecutable(const fs::path& user_directory, const fs::path& release_directory)
{
  for (const ComponentSpec& spec : ReadComponentSpecs(release_directory))
  {
    if (spec.name == "tools")
      return user_directory / "Runtime" / spec.name / spec.version / "moderngekko-port.exe";
  }
  return release_directory / "moderngekko-port.exe";
}

fs::path ResolveToolchainBin(const fs::path& user_directory, const fs::path& release_directory)
{
  for (const ComponentSpec& spec : ReadComponentSpecs(release_directory))
  {
    if (spec.name == "toolchain")
      return user_directory / "Runtime" / spec.name / spec.version / "bin";
  }
  return release_directory / "toolchain" / "bin";
}

// Turn the extracted game into a loadable module, reusing the cached build when one already
// matches this DOL. Runs on a worker thread: the recompiler takes minutes.
bool BuildModule(const fs::path& game_directory, const fs::path& user_directory,
                 const fs::path& release_directory, const std::string& disc_id,
                 const std::string& dol_sha256, BuildState* state)
{
  const fs::path published = PublishedModulePath(release_directory, disc_id);
  const fs::path cache_root = user_directory / "Generated";

  auto fail = [&](std::string message)
  {
    PreserveBuildDiagnostics(cache_root, user_directory);
    // A failed build can leave a half-configured module-build directory behind, and every later
    // attempt then reports errors that come from that cache - a missing GAME_ID, absent coverage
    // ranges - instead of the real cause, so one transient failure locks the user out for good.
    // Clearing this disc's entries means a retry starts from nothing.
    std::error_code ignored;
    fs::remove_all(cache_root / disc_id, ignored);
    std::lock_guard lock(state->mutex);
    state->error = std::move(message);
    state->running = false;
    return false;
  };
  auto note = [&](std::string message)
  {
    std::lock_guard lock(state->mutex);
    state->status = std::move(message);
  };

  const fs::path port_executable = ResolvePortExecutable(user_directory, release_directory);
  if (!fs::is_regular_file(port_executable))
    return fail("the recompiler is missing: " + port_executable.string());

  note("Looking for an existing build");
  if (const std::optional<fs::path> current = FindCurrentModule(cache_root, disc_id, dol_sha256))
  {
    std::error_code copy_error;
    fs::copy_file(*current, published, fs::copy_options::overwrite_existing, copy_error);
    if (copy_error)
      return fail("could not install the module beside the launcher: " + copy_error.message());
    std::lock_guard lock(state->mutex);
    state->finished_module = published;
    state->status = "Recompiled module is ready";
    state->running = false;
    return true;
  }

  // The child gets its own environment rather than mutating the launcher's: PATH gains the bundled
  // toolchain and nothing else in this process is affected.
  SDL_Environment* build_environment = nullptr;
  const std::string path_text = BuildSearchPath(ResolveToolchainBin(user_directory, release_directory));
  if (!path_text.empty())
  {
    build_environment = SDL_CreateEnvironment(true);
    if (build_environment)
    {
      // Windows keeps "Path" and "PATH" as separate variables, and the environment inherited from
      // Explorer spells it "Path". Setting only one leaves the other in place - the inherited one,
      // which holds devkitPro's MSYS cmake - so the child sees both and the inherited value wins.
      // That is why launching from a shell always worked and launching from Explorer never did.
      SDL_SetEnvironmentVariable(build_environment, "PATH", path_text.c_str(), true);
      SDL_SetEnvironmentVariable(build_environment, "Path", path_text.c_str(), true);
    }
  }

  std::error_code ec;
  fs::create_directories(cache_root, ec);
  const fs::path log_path = user_directory / "Logs" / "ModuleBuild.log";
  fs::create_directories(log_path.parent_path(), ec);
  if (ec)
    return fail("could not create the build log directory: " + ec.message());
  RotateBuildLog(log_path);

  SDL_IOStream* log_stream = SDL_IOFromFile(log_path.string().c_str(), "w");
  if (!log_stream)
    return fail("could not open the build log: " + std::string(SDL_GetError()));

  const std::vector<std::string> storage = {
      port_executable.string(),   "build",     game_directory.string(), "--toolchain",
      "gcc",                      "--backend", "llvm",                  "--opt-level",
      "2",                        "--output",  cache_root.string()};
  std::vector<const char*> arguments;
  arguments.reserve(storage.size() + 1);
  for (const std::string& argument : storage)
    arguments.push_back(argument.c_str());
  arguments.push_back(nullptr);

  note("Recompiling the game");
  // Record the child's working conditions in the log before it runs. The identical argv succeeds
  // when launched from a shell and fails from here, so the difference is in the environment, and
  // guessing at which variable costs a full build cycle each time. This makes it a single run.
  if (SDL_IOStream* condition = SDL_IOFromFile(
          (user_directory / "Logs" / "BuildEnvironment.log").string().c_str(), "w"))
  {
    auto dump = [&](const char* name)
    {
      const char* value = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name);
      std::string line = std::string("[env] ") + name + "=" + (value ? value : "<unset>") + "\n";
      SDL_WriteIO(condition, line.data(), line.size());
    };
    std::string command = "[cmd]";
    for (const auto& argument : storage)
    {
      command += ' ';
      command += argument;
    }
    command += "\n";
    SDL_WriteIO(condition, command.data(), command.size());
    std::string where = "[env] cwd=" + fs::current_path().string() + "\n";
    SDL_WriteIO(condition, where.data(), where.size());
    std::string composed = "[env] composed_child_path=" + path_text + "\n";
    SDL_WriteIO(condition, composed.data(), composed.size());
    for (const char* name : {"PATH", "Path", "HOME", "MSYSTEM", "MSYS", "TERM_SESSION_ID", "PWD", "SHELL"})
      dump(name);
    // Every variable, not just the guessed ones: the failing and passing runs differ in something
    // here, and comparing only PATH is how three wrong mechanisms got asserted in one evening. Win32
    // rather than SDL's API - GetEnvironmentStringsW is known good here, and inventing a call would
    // be worse than the bug.
    if (const wchar_t* block = GetEnvironmentStringsW())
    {
      const wchar_t* entry = block;
      while (*entry)
      {
        std::string line = "[env-var] ";
        const wchar_t* cursor = entry;
        for (; *cursor; ++cursor)
          line += (*cursor < 128) ? static_cast<char>(*cursor) : '?';
        line += '\n';
        SDL_WriteIO(condition, line.data(), line.size());
        entry = cursor + 1;
      }
      FreeEnvironmentStringsW(const_cast<wchar_t*>(block));
    }
    SDL_CloseIO(condition);
  }

  state->stage_rank = 0; // a retry starts at step 1 rather than inheriting the last attempt's phase
  state->completed = 0;
  state->total = 1;
  state->bytes_accounted = 0;
  state->bytes_expected = ReadExpectedBuildBytes(user_directory);
  {
    std::lock_guard lock(state->mutex);
    state->build_root = cache_root.string(); // the panel measures this while the build runs
  }

  SDL_Process* process = nullptr;
  std::string process_error;
  {
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties)
    {
      SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, arguments.data());
      SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                            SDL_PROCESS_STDIO_REDIRECT);
      SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log_stream);
      SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
      if (build_environment)
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER,
                               build_environment);
      process = SDL_CreateProcessWithProperties(properties);
      SDL_DestroyProperties(properties);
    }
    if (!process)
      process_error = SDL_GetError();
    SDL_DestroyEnvironment(build_environment);
    // The log stream must stay open until the child has exited. SDL feeds the child's redirected
    // output into it for as long as the child runs; closing it here leaves that transfer writing to
    // a dead stream, the pipe then fills, and the child blocks on write forever - a build that never
    // finishes, with no error and an empty log.
  }
  if (!process)
  {
    SDL_CloseIO(log_stream);
    return fail("could not start the recompiler: " + process_error);
  }

  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  SDL_CloseIO(log_stream); // safe only now, after the child has exited
  if (!waited)
    return fail("the recompiler could not be monitored: " + std::string(SDL_GetError()));
  if (exit_code != 0)
  {
    // Silent startup deaths are the worst case: the child can die before flushing a single byte, so
    // "see the log" points at an empty file. Report the code, decode the ones that mean something,
    // and say plainly when there was no output - that is the difference between a dead end and a fix.
    std::error_code size_error;
    const std::uintmax_t log_bytes = fs::file_size(log_path, size_error);
    const bool empty_log = size_error || log_bytes == 0;
    std::string hint;
    switch (static_cast<unsigned>(exit_code))
    {
    case 0xC0000135u:
      hint = " - a required DLL was not found";
      break;
    case 0xC0000142u:
      hint = " - a required DLL failed to initialise";
      break;
    case 0xC0000005u:
      hint = " - the recompiler crashed (access violation)";
      break;
    default:
      break;
    }
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(exit_code));
    return fail("the recompiler failed with exit code " + std::string(code) + hint +
                (empty_log ? " and produced no output at all" : " - see Logs/ModuleBuild.log"));
  }

  const std::optional<fs::path> built = FindCurrentModule(cache_root, disc_id, dol_sha256);
  if (!built)
    return fail("the recompiler reported success but published no module");
  // Calibrate for next time from what this build actually accounted for, then finish the bar: the
  // display tops out at 99% while work is in flight, so 100% means the module really was published.
  {
    const std::uintmax_t measured = state->bytes_accounted.load();
    if (measured > 0)
    {
      std::ofstream out(ExpectedBuildBytesPath(user_directory), std::ios::trunc);
      out << measured;
      state->bytes_expected = measured;
      state->bytes_accounted = measured;
    }
  }
  fs::copy_file(*built, published, fs::copy_options::overwrite_existing, ec);
  if (ec)
    return fail("could not install the compiled module beside the launcher: " + ec.message());
  {
    std::lock_guard lock(state->mutex);
    state->finished_module = published;
    state->status = "Recompiled module is ready";
  }
  state->running = false;
  return true;
}

std::optional<fs::path> PrepareDisc(const fs::path& image, const fs::path& user_directory,
                                    const fs::path& release_directory, ExtractionState* state,
                                    std::string* error)
{
  std::unique_ptr<DiscIO::Volume> source = DiscIO::CreateVolume(image.string());
  if (!source)
  {
    *error = "Dolphin rejected the selected disc image";
    return std::nullopt;
  }
  const std::string source_id = source->GetGameID(source->GetGamePartition());
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (source_id == MODERNGEKKO_REQUIRED_DISC_ID)
    return image;
#endif
#if defined(MODERNGEKKO_DISC_PREPARER_FILENAME) && \
    defined(MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID) && defined(MODERNGEKKO_REQUIRED_DISC_ID)
  if (source_id != MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID)
  {
    *error = "expected clean " MODERNGEKKO_ACCEPTED_SOURCE_DISC_ID " or patched "
             MODERNGEKKO_REQUIRED_DISC_ID "; selected " + source_id;
    return std::nullopt;
  }
  const fs::path output = user_directory / "Setup" / "SonicRidersTE-2.4.6.1.iso";
  std::error_code ec;
  fs::create_directories(output.parent_path(), ec);
  if (ec)
  {
    *error = "can't create setup directory: " + ec.message();
    return std::nullopt;
  }
  {
    std::lock_guard lock(state->mutex);
    state->status = "Applying Tournament Edition 2.4.6.1";
  }
  const fs::path preparer =
      SiblingExecutable(release_directory, MODERNGEKKO_DISC_PREPARER_FILENAME);
  const std::array<std::string, 5> storage = {
      preparer.string(), "--input", image.string(), "--output", output.string()};
  std::array<const char*, 6> arguments{};
  for (std::size_t i = 0; i < storage.size(); ++i)
    arguments[i] = storage[i].c_str();
  SDL_Process* process = SDL_CreateProcess(arguments.data(), false);
  if (!process)
  {
    *error = "can't start Tournament Edition patcher: " + std::string(SDL_GetError());
    return std::nullopt;
  }
  int exit_code = 1;
  const bool waited = SDL_WaitProcess(process, true, &exit_code);
  SDL_DestroyProcess(process);
  if (!waited || exit_code != 0)
  {
    *error = "Tournament Edition patching failed";
    return std::nullopt;
  }
  return output;
#else
  // No disc preparer is built in, so the exact-match check above is the whole policy. This message
  // used to be inherited from another game's launcher and told the player nothing useful, so it now
  // names the one release that is accepted and what the inserted disc actually reported.
#if defined(MODERNGEKKO_REQUIRED_DISC_ID)
  const std::string wanted = MODERNGEKKO_REQUIRED_DISC_ID;
#else
  const std::string wanted = "unset";
#endif
  *error = "this launcher accepts only the US release of Pokemon Colosseum, game ID " + wanted +
           " - the selected disc reported " +
           (source_id.empty() ? std::string("no game ID") : source_id);
  return std::nullopt;
#endif
}

bool ExtractDisc(const fs::path& image, const fs::path& user_directory,
                 const fs::path& release_directory, ExtractionState* state)
{
  auto fail = [&](std::string message)
  {
    std::lock_guard lock(state->mutex);
    state->error = std::move(message);
    state->running = false;
    return false;
  };

  {
    std::lock_guard lock(state->mutex);
    state->status = "Opening " + image.filename().string();
  }
  std::string preparation_error;
  const std::optional<fs::path> prepared =
      PrepareDisc(image, user_directory, release_directory, state, &preparation_error);
  if (!prepared)
    return fail(std::move(preparation_error));
  std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(prepared->string());
  if (!volume)
    return fail("Dolphin rejected the prepared disc image");

  const DiscIO::Partition partition = volume->GetGamePartition();
  const DiscIO::FileSystem* filesystem = volume->GetFileSystem(partition);
  if (!filesystem || !filesystem->IsValid())
    return fail("Dolphin could not read the game partition filesystem");

  std::string disc_id = volume->GetGameID(partition);
  if (disc_id.size() != 6)
    return fail("the selected image has an invalid disc ID");
#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
    return fail("this frontend requires disc ID " MODERNGEKKO_REQUIRED_DISC_ID "; selected " +
                disc_id);
#endif
#ifdef MODERNGEKKO_PORTABLE_DEFAULT_GAME
  const fs::path games_directory = release_directory;
  const fs::path output = games_directory / "Game";
#else
  const fs::path games_directory = user_directory / "games";
  const fs::path output = games_directory / disc_id;
#endif
  std::error_code ec;
  if (fs::is_directory(output, ec))
  {
    bool changed = false;
    std::string patch_error;
    if (!ApplyBundledDolPatch(output, release_directory, &changed, &patch_error))
      return fail("existing extraction patching failed: " + patch_error);
  }
  const auto existing = moderngekko::InspectGame(output);
  if (existing && RequiredGameError(*existing.metadata).empty())
  {
    std::string error;
    if (!WriteDefaultGame(user_directory, release_directory, output, &error))
      return fail(error);
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Using existing extraction";
    state->running = false;
    return true;
  }

  const fs::path staging = games_directory / (disc_id + ".extracting");
  fs::remove_all(staging, ec);
  fs::create_directories(staging / "files", ec);
  if (ec)
    return fail("can't create extraction directory: " + ec.message());

  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting system data";
  }
  if (!DiscIO::ExportSystemData(*volume, partition, staging.string()))
  {
    fs::remove_all(staging, ec);
    return fail("Dolphin failed while extracting the disc system data");
  }

  state->total = std::max(1u, filesystem->GetRoot().GetTotalChildren());
  {
    std::lock_guard lock(state->mutex);
    state->status = "Extracting game files";
  }
  DiscIO::ExportDirectory(*volume, partition, filesystem->GetRoot(), true, "",
                          (staging / "files").string(),
                          [state](const std::string& path)
                          {
                            ++state->completed;
                            std::lock_guard lock(state->mutex);
                            state->status = "Extracting " + path;
                            return false;
                          });

  bool dol_changed = false;
  std::string dol_patch_error;
  if (!ApplyBundledDolPatch(staging, release_directory, &dol_changed, &dol_patch_error))
  {
    fs::remove_all(staging, ec);
    return fail("extracted DOL patching failed: " + dol_patch_error);
  }

  const auto inspected = moderngekko::InspectGame(staging);
  if (!inspected)
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + inspected.error);
  }
  if (const std::string identity_error = RequiredGameError(*inspected.metadata);
      !identity_error.empty())
  {
    fs::remove_all(staging, ec);
    return fail("extracted game validation failed: " + identity_error);
  }

  fs::remove_all(output, ec);
  ec.clear();
  fs::rename(staging, output, ec);
  if (ec)
    return fail("can't publish extracted game: " + ec.message());
  std::string error;
  if (!WriteDefaultGame(user_directory, release_directory, output, &error))
    return fail(error);

  {
    std::lock_guard lock(state->mutex);
    state->finished_game = output;
    state->status = "Extraction complete";
  }
  state->running = false;
  return true;
}

void SDLCALL FileDialogCallback(void* userdata, const char* const* filelist, int)
{
  auto* state = static_cast<DialogState*>(userdata);
  std::lock_guard lock(state->mutex);
  if (!filelist)
    state->error = SDL_GetError();
  else if (filelist[0])
    state->selected = filelist[0];
}

fs::path SiblingRunner(const char* argv0)
{
  std::error_code ec;
  const fs::path self = fs::weakly_canonical(argv0, ec);
  fs::path runner = MODERNGEKKO_RUNNER_FILENAME;
#if defined(_WIN32)
  runner += ".exe";
#endif
  const fs::path sibling = self.parent_path() / runner;
  return fs::is_regular_file(sibling) ? sibling : runner;
}
} // namespace

int main(int argc, char** argv)
{
  bool use_wayland = false;
  std::optional<fs::path> extract_only;
  bool fetch_components = false;
  for (int i = 1; i < argc; ++i)
  {
    if (std::string_view(argv[i]) == "-X11" || std::string_view(argv[i]) == "--x11")
      use_wayland = false;
    else if (std::string_view(argv[i]) == "--wayland")
      use_wayland = true;
    else if (std::string_view(argv[i]) == "--extract" && i + 1 < argc)
      extract_only = argv[++i];
    else if (std::string_view(argv[i]) == "--fetch-components")
      fetch_components = true;
  }

  const fs::path user_directory = DefaultUserDirectory();
  const fs::path release_directory = ExecutableDirectory(argv[0]);
  if (fetch_components)
  {
    // Headless acquisition: installs whatever components.txt lists and is not already present,
    // from the archives beside the launcher. Exists so extraction can be exercised without a
    // display, and on a machine with no network.
    SetupState setup;
    setup.running = true;
    const bool success = EnsureComponents(user_directory, release_directory, &setup);
    std::lock_guard lock(setup.mutex);
    if (!success)
      std::cerr << "component fetch failed: " << setup.error << '\n';
    else
      std::cout << "components ready\n";
    return success ? 0 : 1;
  }
  if (extract_only)
  {
    ExtractionState extraction;
    extraction.running = true;
    const bool success =
        ExtractDisc(*extract_only, user_directory, release_directory, &extraction);
    std::lock_guard lock(extraction.mutex);
    if (!success)
      std::cerr << "extraction failed: " << extraction.error << '\n';
    else
      std::cout << extraction.status << ": " << *extraction.finished_game << '\n';
    return success ? 0 : 1;
  }

  auto config = moderngekko::frontend::LoadConfig(user_directory, true);
  if (!config)
  {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             "Invalid " MODERNGEKKO_FRONTEND_NAME " config.ini",
                             config.error.c_str(), nullptr);
    return 2;
  }

#if defined(__linux__)
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, use_wayland ? "wayland" : "x11");
#endif
  SDL_setenv_unsafe("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    return 1;

  const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  SDL_Window* window = SDL_CreateWindow(MG_DISPLAY_BRAND, static_cast<int>(820 * scale),
                                        static_cast<int>(700 * scale),
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window)
  {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, "vulkan");
  if (!renderer)
    renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer)
  {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::vector<fs::path> images = FindDiscImages();
  std::optional<fs::path> selected_image = images.empty() ? std::nullopt : std::optional(images[0]);
  fs::path current_game = ReadDefaultGame(user_directory, release_directory);
  bool current_dol_changed = false;
  std::string current_dol_error;
  auto current_metadata =
      ApplyBundledDolPatch(current_game, release_directory, &current_dol_changed,
                           &current_dol_error) ?
          moderngekko::InspectGame(current_game) :
          moderngekko::GameInspectResult{{}, current_dol_error};
  if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
    current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
  const auto& resolutions = moderngekko::frontend::SupportedResolutions();
  const auto& graphics_backends =
      moderngekko::frontend::SupportedGraphicsBackends();
  bool show_fps_in_title = config.show_fps_in_title;
  bool fullscreen = config.fullscreen;
  std::array<char, 31> netplay_nickname{};
  std::array<char, 256> netplay_address{};
  std::snprintf(netplay_nickname.data(), netplay_nickname.size(), "%s",
                config.netplay_nickname.c_str());
  std::snprintf(netplay_address.data(), netplay_address.size(), "%s",
                config.netplay_address.c_str());
  int netplay_port = config.netplay_port;
  bool automatic_buffer = config.netplay_buffer == "auto";
  int manual_buffer = automatic_buffer ? 5 : std::stoi(config.netplay_buffer);
  int resolution_index = 0;
  for (std::size_t i = 0; i < resolutions.size(); ++i)
  {
    if (config.resolution == resolutions[i].text)
      resolution_index = static_cast<int>(i);
  }
  int graphics_backend_index = 0;
  for (std::size_t i = 0; i < graphics_backends.size(); ++i)
  {
    if (config.graphics_backend == graphics_backends[i].value)
      graphics_backend_index = static_cast<int>(i);
  }

  // StateSaves is Dolphin's own state directory, so states written in-game land
  // here without any extra configuration and show up on the next launch.
  //
  // Listed once at startup rather than every frame, so the dropdown does not
  // hit the filesystem on each redraw. A state written while the launcher is
  // open therefore appears on the next launch, which is the case that matters:
  // states are written by the running game, not by the launcher.
  const std::vector<fs::path> launcher_savestates =
      moderngekko::frontend::ListLauncherSavestates(user_directory / "StateSaves");
  // Loading is always explicit. StateSaves is shared by games, so selecting the
  // newest file automatically could try to resume a different title.
  int selected_savestate = -1;

  DialogState dialog;
  std::vector<ControllerOption> controllers = EnumerateControllers();
  bool controller_profile_exists = moderngekko::frontend::ControllerConfigExists(user_directory);
  std::vector<std::string> configured_controllers =
      moderngekko::frontend::ReadConfiguredControllers(user_directory);
  std::string selected_controller =
      configured_controllers.empty() ? config.controller : configured_controllers.front();
  int controller_index = FindController(controllers, selected_controller);
  std::string controller_status;
  const auto select_controller = [&](int index)
  {
    std::string message;
    if (!moderngekko::frontend::GenerateControllerConfig(user_directory, controllers[index].device,
                                                         &message))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(message);
      return false;
    }
    std::string error;
    if (!moderngekko::frontend::SaveConfig(user_directory, resolutions[resolution_index].text,
                                           show_fps_in_title, controllers[index].device, &error))
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = std::move(error);
      return false;
    }
    selected_controller = controllers[index].device;
    configured_controllers = {selected_controller};
    config.controller = selected_controller;
    config.controllers = configured_controllers;
    config.resolution = resolutions[resolution_index].text;
    config.show_fps_in_title = show_fps_in_title;
    controller_profile_exists = true;
    controller_status = std::move(message);
    return true;
  };
  const auto ensure_controller = [&]
  {
    if (moderngekko::frontend::ControllerConfigExists(user_directory))
    {
      controller_profile_exists = true;
      configured_controllers = moderngekko::frontend::ReadConfiguredControllers(user_directory);
      if (configured_controllers.empty())
      {
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::string(CONTROLLER_PROFILE_NAME) +
                       " has no configured controller device";
        return false;
      }
      selected_controller = configured_controllers.front();
      controller_index = FindController(controllers, selected_controller);
      controller_status = "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
      return true;
    }
    if (controller_index < 0)
    {
      std::lock_guard lock(dialog.mutex);
      dialog.error = "Connect an SDL-compatible controller before playing";
      return false;
    }
    return select_controller(controller_index);
  };
  const auto refresh_controllers = [&]
  {
    controllers = EnumerateControllers();
    controller_index = FindController(controllers, selected_controller);
    if (!controller_profile_exists)
    {
      if (controller_index < 0 && !controllers.empty())
      {
        controller_index = 0;
        selected_controller = controllers.front().device;
      }
      if (controller_index >= 0)
        select_controller(controller_index);
      else
        controller_status = "No SDL gamepad detected";
    }
    else
    {
      controller_status = "Using existing " + std::string(CONTROLLER_PROFILE_NAME);
    }
  };
  refresh_controllers();
  ExtractionState extraction;
  std::jthread extraction_thread;
  BuildState build;
  std::jthread build_thread;
  SetupState setup;
  std::jthread setup_thread;
  enum class LaunchMode
  {
    None,
    Solo,
    Host,
    Join,
  };
  bool done = false;
  LaunchMode launch_mode = LaunchMode::None;
  while (!done)
  {
    bool controllers_changed = false;
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        done = true;
      if (event.type == SDL_EVENT_JOYSTICK_ADDED || event.type == SDL_EVENT_JOYSTICK_REMOVED ||
          event.type == SDL_EVENT_GAMEPAD_REMAPPED)
      {
        controllers_changed = true;
      }
    }
    if (controllers_changed)
      refresh_controllers();

    {
      std::lock_guard lock(dialog.mutex);
      if (dialog.selected)
      {
        selected_image = std::move(dialog.selected);
        dialog.selected.reset();
      }
    }
    {
      std::lock_guard lock(extraction.mutex);
      if (extraction.finished_game)
      {
        current_game = *extraction.finished_game;
        current_metadata = moderngekko::InspectGame(current_game);
        if (current_metadata && !RequiredGameError(*current_metadata.metadata).empty())
          current_metadata = {{}, RequiredGameError(*current_metadata.metadata)};
        extraction.finished_game.reset();
      }
    }

    // Fetch anything the recompiler needs that is not already present. On a machine that has it all
    // this completes immediately; otherwise the download happens before play can be offered.
    if (!setup.running && !setup.complete && !setup_thread.joinable())
    {
      setup.running = true;
      {
        std::lock_guard setup_lock(setup.mutex);
        setup.phase = "Checking what is already installed";
      }
      setup_thread = std::jthread([user_directory, release_directory, &setup]
                                  { EnsureComponents(user_directory, release_directory, &setup); });
    }

    // Build once both halves are ready: the game extracted and the recompiler available. Checked
    // every frame, so whichever finishes last is the one that triggers the build.
    {
      bool needs_build = false;
      {
        std::lock_guard build_lock(build.mutex);
        needs_build = !build.finished_module.has_value();
      }
      if (setup.complete && needs_build && !build.running && !build.attempted &&
          current_metadata && current_metadata.metadata)
      {
        // The latch is the whole point. This condition is checked every frame, and after a failed
        // build it is true again - no module, nothing running - so without it the build restarts
        // forever. Because a failed build clears its cache, every retry begins from nothing: the
        // progress bar resets, the log truncates to empty and no objects survive. One attempt, then
        // wait for the operator to ask again.
        build.attempted = true;
        build.running = true;
        {
          std::lock_guard build_lock(build.mutex);
          build.error.clear();
          build.log_tail.clear();
          build.status = "Preparing the recompiled module";
        }
        const fs::path build_game = current_game;
        const std::string build_disc_id = current_metadata.metadata->disc_id;
        const std::string build_dol = current_metadata.metadata->dol_sha256;
        build_thread = std::jthread([build_game, user_directory, release_directory, build_disc_id,
                                     build_dol, &build]
                                    {
                                      BuildModule(build_game, user_directory, release_directory,
                                                  build_disc_id, build_dol, &build);
                                    });
      }
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(MG_DISPLAY_BRAND " Launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(MG_DISPLAY_BRAND);
    ImGui::Separator();

    if (current_metadata)
    {
      ImGui::Text("Ready: %s [%s]", current_metadata.metadata->game_name.c_str(),
                  current_metadata.metadata->disc_id.c_str());
      if (!launcher_savestates.empty())
      {
        ImGui::TextUnformatted("Start from savestate");
        const std::string savestate_preview =
            selected_savestate >= 0 ?
                moderngekko::frontend::LauncherSavestateLabel(
                    launcher_savestates[static_cast<std::size_t>(selected_savestate)],
                    selected_savestate == 0) :
                "Normal boot";
        if (ImGui::BeginCombo("##launch_savestate", savestate_preview.c_str()))
        {
          const bool normal_boot_selected = selected_savestate < 0;
          if (ImGui::Selectable("Normal boot", normal_boot_selected))
            selected_savestate = -1;
          if (normal_boot_selected)
            ImGui::SetItemDefaultFocus();

          for (std::size_t i = 0; i < launcher_savestates.size(); ++i)
          {
            const std::string label =
                moderngekko::frontend::LauncherSavestateLabel(launcher_savestates[i], i == 0);
            const bool selected = selected_savestate == static_cast<int>(i);
            if (ImGui::Selectable(label.c_str(), selected))
              selected_savestate = static_cast<int>(i);
            if (selected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        // Netplay starts every player from the same boot, so a state chosen by
        // one side would desync the session immediately.
        ImGui::TextDisabled("Solo play only.");
        ImGui::Spacing();
      }
      bool module_ready = false;
      {
        std::lock_guard build_lock(build.mutex);
        module_ready = build.finished_module.has_value();
      }
      if (!module_ready && current_metadata && current_metadata.metadata)
        module_ready = fs::is_regular_file(
            PublishedModulePath(release_directory, current_metadata.metadata->disc_id));
      ImGui::BeginDisabled(!module_ready);
      if (ImGui::Button("Play", ImVec2(180 * scale, 42 * scale)))
      {
        if (ensure_controller())
        {
          launch_mode = LaunchMode::Solo;
          done = true;
        }
      }
      ImGui::EndDisabled();
    }
    else
    {
      ImGui::TextUnformatted("No extracted game is configured yet.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Graphics backend");
    if (ImGui::BeginCombo("##graphics_backend",
                          graphics_backends[graphics_backend_index].text))
    {
      for (std::size_t i = 0; i < graphics_backends.size(); ++i)
      {
        const bool selected = graphics_backend_index == static_cast<int>(i);
        if (ImGui::Selectable(graphics_backends[i].text, selected))
        {
          const std::string previous = config.graphics_backend;
          config.graphics_backend = graphics_backends[i].value;
          config.resolution = resolutions[resolution_index].text;
          config.show_fps_in_title = show_fps_in_title;
          config.controller = selected_controller;
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, config, &error))
          {
            graphics_backend_index = static_cast<int>(i);
          }
          else
          {
            config.graphics_backend = previous;
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::Spacing();
    const bool previous_fullscreen = fullscreen;
    if (ImGui::Checkbox("Start in fullscreen", &fullscreen))
    {
      config.fullscreen = fullscreen;
      config.resolution = resolutions[resolution_index].text;
      config.show_fps_in_title = show_fps_in_title;
      config.controller = selected_controller;
      std::string error;
      if (!moderngekko::frontend::SaveConfig(user_directory, config, &error))
      {
        fullscreen = previous_fullscreen;
        config.fullscreen = previous_fullscreen;
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::move(error);
      }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Internal resolution (Dolphin EFB upscale)");
    if (ImGui::BeginCombo("##resolution", resolutions[resolution_index].text))
    {
      for (std::size_t i = 0; i < resolutions.size(); ++i)
      {
        const bool selected = resolution_index == static_cast<int>(i);
        if (ImGui::Selectable(resolutions[i].text, selected))
        {
          std::string error;
          if (moderngekko::frontend::SaveConfig(user_directory, resolutions[i].text,
                                                show_fps_in_title, selected_controller, &error))
          {
            resolution_index = static_cast<int>(i);
            config.resolution = resolutions[i].text;
          }
          else
          {
            std::lock_guard lock(dialog.mutex);
            dialog.error = std::move(error);
          }
        }
      }
      ImGui::EndCombo();
    }
    const bool previous_show_fps_in_title = show_fps_in_title;
    if (ImGui::Checkbox("Show FPS in window title", &show_fps_in_title))
    {
      std::string error;
      if (!moderngekko::frontend::SaveConfig(user_directory, resolutions[resolution_index].text,
                                             show_fps_in_title, selected_controller, &error))
      {
        show_fps_in_title = previous_show_fps_in_title;
        std::lock_guard lock(dialog.mutex);
        dialog.error = std::move(error);
      }
      else
      {
        config.show_fps_in_title = show_fps_in_title;
      }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Controller profile");
    const char* controller_preview = controller_index >= 0
                                         ? controllers[controller_index].label.c_str()
                                     : selected_controller.empty() ? "No SDL gamepad detected"
                                                                   : selected_controller.c_str();
    if (ImGui::BeginCombo("##controller", controller_preview))
    {
      for (std::size_t i = 0; i < controllers.size(); ++i)
      {
        const bool selected = controller_index == static_cast<int>(i);
        if (ImGui::Selectable(controllers[i].label.c_str(), selected))
        {
          controller_index = static_cast<int>(i);
          selected_controller = controllers[i].device;
          controller_status =
              controller_profile_exists
                  ? "Existing " + std::string(CONTROLLER_PROFILE_NAME) + " unchanged"
                  : "Ready to generate controller profile";
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::BeginDisabled(controller_index < 0);
    const std::string replace_profile =
        "Replace with generated " + std::string(GENERATED_PROFILE_LABEL);
    if (ImGui::Button(replace_profile.c_str()))
      select_controller(controller_index);
    ImGui::EndDisabled();
    ImGui::Spacing();
    // The netplay section came from the upstream template. Colosseum has no network path for it to
    // drive, and an untested feature in a public launcher is a support burden, so the UI is gone.
    // The runner still accepts --netplay-* arguments if this is ever revisited.
    ImGui::Spacing();
    ImGui::TextUnformatted("Game disc image");
    if (selected_image)
      ImGui::TextWrapped("%s", selected_image->string().c_str());
    else
      ImGui::TextDisabled("No ISO, WBFS, or RVZ selected");

    if (!extraction.running)
    {
      if (ImGui::Button("Browse for ISO / WBFS / RVZ"))
      {
        static constexpr SDL_DialogFileFilter filters[] = {
            {"Disc images", "iso;wbfs;rvz"},
            {"ISO", "iso"},
            {"WBFS", "wbfs"},
            {"RVZ", "rvz"}};
        const std::string documents = DocumentsDirectory().string();
        SDL_ShowOpenFileDialog(FileDialogCallback, &dialog, window, filters,
                               static_cast<int>(std::size(filters)), documents.c_str(), false);
      }
      if (selected_image)
      {
        ImGui::SameLine();
        if (ImGui::Button("Extract and Build"))
        {
          build.attempted = false; // an explicit request clears the latch, allowing one more attempt
          extraction.completed = 0;
          extraction.total = 1;
          extraction.running = true;
          {
            std::lock_guard lock(extraction.mutex);
            extraction.error.clear();
            extraction.finished_game.reset();
          }
          const fs::path image = *selected_image;
          extraction_thread = std::jthread([image, user_directory, release_directory, &extraction]
                                           {
                                             ExtractDisc(image, user_directory,
                                                         release_directory, &extraction);
                                           });
        }
      }
    }
    else
    {
      const float progress =
          std::min(1.0f, static_cast<float>(extraction.completed.load()) / extraction.total.load());
      ImGui::ProgressBar(progress, ImVec2(-1, 0));
    }

    {
      std::lock_guard lock(extraction.mutex);
      if (!extraction.status.empty())
        ImGui::TextWrapped("%s", extraction.status.c_str());
      if (!extraction.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", extraction.error.c_str());
    }
    {
      std::lock_guard lock(dialog.mutex);
      if (!dialog.error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", dialog.error.c_str());
    }
    // First-run acquisition status. Silent once everything is present, which is the normal case.
    {
      std::string setup_phase;
      std::string setup_error;
      {
        std::lock_guard setup_lock(setup.mutex);
        setup_phase = setup.phase;
        setup_error = setup.error;
      }
      const bool setup_running = setup.running.load();
      if (!setup.complete || !setup_error.empty())
      {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Preparing the recompiler");
        const unsigned long long received = setup.received.load();
        const unsigned long long total = setup.total.load();
        if (total > 0)
        {
          ImGui::ProgressBar(static_cast<float>(received) / static_cast<float>(total),
                             ImVec2(-1, 0));
          ImGui::TextWrapped("%.1f / %.1f MB", static_cast<double>(received) / 1048576.0,
                             static_cast<double>(total) / 1048576.0);
        }
        else if (setup_running)
        {
          ImGui::TextWrapped("Working...");
        }
        if (!setup_phase.empty())
          ImGui::TextWrapped("%s", setup_phase.c_str());
        if (!setup.complete.load() && setup_error.empty() && !setup_running)
          ImGui::TextDisabled(
              "Play becomes available once the recompiler has what it needs.");
        if (!setup_error.empty())
          ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", setup_error.c_str());
      }
    }

    // Recompilation progress, polled from the build log rather than piped, so a crashed or killed
    // recompiler still leaves its diagnostics on disk for the report.
    {
      static unsigned build_log_tick = 0;
      if (build.running.load() && (build_log_tick++ % 15u) == 0u)
        RefreshBuildLog(&build, user_directory / "Logs" / "ModuleBuild.log");
      // Measure the build directory a couple of times a second, attributing each path's growth once.
      // A full walk of ~20k files costs tens of milliseconds, so it is throttled well below the frame
      // rate. The seen map is owned by this thread and reset on the rising edge of a new build.
      static unsigned build_size_tick = 0;
      static bool was_running = false;
      static std::map<std::string, std::uintmax_t> seen_files;
      if (build.running.load() && !was_running)
      {
        seen_files.clear();
        build.bytes_accounted = 0;
      }
      was_running = build.running.load();
      if (build.running.load() && (build_size_tick++ % 60u) == 0u)
      {
        std::string measured_root;
        {
          std::lock_guard root_lock(build.mutex);
          measured_root = build.build_root;
        }
        if (!measured_root.empty())
          build.bytes_accounted = AccountBuildBytes(fs::path(measured_root), seen_files);
      }
      bool active = false;
      bool has_error = false;
      bool has_log = false;
      std::string status;
      int stage_rank = 0;
      std::string error;
      std::string log_tail;
      {
        std::lock_guard build_lock(build.mutex);
        status = build.status;
        stage_rank = build.stage_rank;
        error = build.error;
        log_tail = build.log_tail;
        active = build.running.load();
        has_error = !error.empty();
        has_log = !log_tail.empty();
      }
      if (active || has_error || has_log)
      {
        ImGui::Spacing();
        ImGui::Separator();
        // One bar for the whole build, 0 to 100, and it only ever advances. The phase is shown as
        // text: several bars appearing, filling and disappearing is what made the old panel look
        // like it was going backwards.
        static bool was_active = false;
        static bool show_details = false;
        if (active && !was_active)
          build.started_ms = SDL_GetTicks(); // rising edge: one clock for the entire build
        was_active = active;

        // Bytes on disk against the size a complete build of this disc reaches. A measurement, not a
        // model: every phase writes files, so it rises continuously through all of them and never
        // restarts. This is why the bar can be trusted where a per-phase or weighted one could not.
        const unsigned long long megabyte = 1024ull * 1024ull;
        const unsigned long long accounted = build.bytes_accounted.load();
        const unsigned long long expected =
            std::max<unsigned long long>(1ull, build.bytes_expected.load());
        // Capped at 99 while a build is in flight: only a published module gets to claim 100.
        const float raw =
            100.0f * static_cast<float>(accounted) / static_cast<float>(expected);
        const float percent = std::min(active ? 99.0f : 100.0f, raw);
        char overlay[24];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", percent);
        ImGui::ProgressBar(percent / 100.0f, ImVec2(-1, 0), overlay);
        ImGui::TextDisabled("%llu / %llu MB", accounted / megabyte, expected / megabyte);
        if (stage_rank > 0)
          ImGui::Text("%s", kStageNames[stage_rank]);
        else if (active)
          ImGui::TextUnformatted("Starting up");
        if (active)
        {
          const unsigned long long started = build.started_ms.load();
          if (started != 0)
          {
            const unsigned long long elapsed = (SDL_GetTicks() - started) / 1000ull;
            ImGui::TextDisabled("Elapsed %llu:%02llu", elapsed / 60ull, elapsed % 60ull);
          }
        }
        if (!status.empty())
          ImGui::TextWrapped("%s", status.c_str());
        if (has_error)
          ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", error.c_str());
        // Details are opt-in: a wall of recompiler output streaming past is noise for a normal user,
        // but it is what you need when something goes wrong.
        if (has_log)
        {
          ImGui::Checkbox("Show details", &show_details);
          if (show_details)
          {
            ImGui::BeginChild("modulebuildlog", ImVec2(0, 140 * scale), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(log_tail.c_str());
            ImGui::EndChild();
          }
        }
      }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Controller: %s", controller_status.c_str());
    ImGui::End();

    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  if (extraction_thread.joinable())
    extraction_thread.join();
  if (build_thread.joinable())
    build_thread.join();

  int result = 0;
  if (launch_mode != LaunchMode::None)
  {
    std::string launch_error;
    if (!WriteDefaultGame(user_directory, release_directory, current_game,
                          &launch_error))
    {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(), window);
      result = 1;
    }
    else
    {
      std::vector<std::string> argument_storage = {SiblingRunner(argv[0]).string(), "--game",
                                                   current_game.string(), "--user-dir",
                                                   user_directory.string()};
      // Solo only: see the note by the dropdown.
      if (launch_mode == LaunchMode::Solo && selected_savestate >= 0 &&
          static_cast<std::size_t>(selected_savestate) < launcher_savestates.size())
      {
        argument_storage.emplace_back("--load-state");
        argument_storage.emplace_back(
            moderngekko::frontend::PathText(
                launcher_savestates[static_cast<std::size_t>(selected_savestate)]));
      }
      if (launch_mode == LaunchMode::Host)
        argument_storage.emplace_back("--netplay-host");
      else if (launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-join");
        argument_storage.emplace_back(config.netplay_address);
      }
      if (launch_mode == LaunchMode::Host || launch_mode == LaunchMode::Join)
      {
        argument_storage.emplace_back("--netplay-port");
        argument_storage.emplace_back(std::to_string(config.netplay_port));
        argument_storage.emplace_back("--nickname");
        argument_storage.emplace_back(config.netplay_nickname);
        argument_storage.emplace_back("--buffer");
        argument_storage.emplace_back(config.netplay_buffer);
        for (const std::string& controller : config.controllers)
        {
          argument_storage.emplace_back("--controller");
          argument_storage.emplace_back(controller);
        }
      }
      if (use_wayland)
        argument_storage.emplace_back("--wayland");
#if defined(__linux__)
      else
        argument_storage.emplace_back("-X11");
#endif
      std::vector<const char*> arguments;
      arguments.reserve(argument_storage.size() + 1);
      for (const std::string& argument : argument_storage)
        arguments.push_back(argument.c_str());
      arguments.push_back(nullptr);
      const std::filesystem::path log_path =
          user_directory / "Logs" / MODERNGEKKO_LOG_FILENAME;
      std::error_code log_error;
      std::filesystem::create_directories(log_path.parent_path(), log_error);
      SDL_IOStream* log_stream =
          log_error ? nullptr : SDL_IOFromFile(log_path.string().c_str(), "w");
      SDL_Process* process = nullptr;
      std::string process_error;
      if (log_stream)
      {
        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties)
        {
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                                 arguments.data());
          SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                                SDL_PROCESS_STDIO_REDIRECT);
          SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log_stream);
          SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN,
                                 true);
          process = SDL_CreateProcessWithProperties(properties);
          SDL_DestroyProperties(properties);
        }
        if (!process)
          process_error = SDL_GetError();
        SDL_CloseIO(log_stream);
      }
      else
      {
        process = SDL_CreateProcess(arguments.data(), false);
        if (!process)
          process_error = SDL_GetError();
      }
      if (!process)
      {
        launch_error = "Could not start " + SiblingRunner(argv[0]).string() + ": " + process_error;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", launch_error.c_str(),
                                 window);
        result = 1;
      }
      else
      {
        SDL_HideWindow(window);
        int exit_code = 1;
        const bool waited = SDL_WaitProcess(process, true, &exit_code);
        SDL_DestroyProcess(process);
        if (!waited || exit_code != 0)
        {
          SDL_ShowWindow(window);
          if (!waited)
          {
            launch_error =
                "The game process could not be monitored: " + std::string(SDL_GetError());
          }
          else if (launch_mode == LaunchMode::Join)
          {
            switch (static_cast<moderngekko::frontend::NetplayExitCode>(exit_code))
            {
            case moderngekko::frontend::NetplayExitCode::VersionMismatch:
              launch_error = "The host is running an incompatible netplay build. Both "
                             "players must use the same release.";
              break;
            case moderngekko::frontend::NetplayExitCode::CompatibilityMismatch:
              launch_error = "The extracted game or recomp module does not match the "
                             "host. Both players need the same game revision and "
                             "release.";
              break;
            case moderngekko::frontend::NetplayExitCode::RoomFull:
              launch_error = "All four controller slots are already occupied.";
              break;
            case moderngekko::frontend::NetplayExitCode::GameRunning:
              launch_error = "The host has already started the game.";
              break;
            case moderngekko::frontend::NetplayExitCode::ServerFull:
              launch_error = "The netplay server is full.";
              break;
            case moderngekko::frontend::NetplayExitCode::NicknameRejected:
              launch_error = "The nickname was rejected by the host.";
              break;
            default:
              launch_error = "Could not reach the netplay host. Check the host name, UDP "
                             "port, and firewall.";
              break;
            }
          }
          else if (launch_mode == LaunchMode::Host)
          {
            launch_error = "Could not create the netplay session. Check the UDP port "
                           "and firewall settings.";
          }
          else
          {
            launch_error = "The game process exited with code " + std::to_string(exit_code) + ".";
          }
          if (!log_error)
            launch_error += "\n\nDetails were written to:\n" + log_path.string();
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Session failed", launch_error.c_str(),
                                   window);
          result = 1;
        }
      }
    }
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
