// SteamInputREV-AI — standalone driver for Valve's Steam Input engine.
//
// Loads the real `steamclient` library from an existing Steam installation
// and drives its controller engine through the same C++ ABI games use,
// without the Steam client running and without any credentials.
//
// ABI provenance — vtable order and struct layouts transcribed from the
// Steamworks SDK 1.65 headers vendored by ValveSoftware/Proton at
// lsteamclient/steamworks_sdk_165/:
//   isteamclient.h  sha256 cf409526affe5956b561b40b71feb8600acae5971de91115b42a79974ab08387
//                   -> ISteamClient  @ "SteamClient023"
//   isteaminput.h   sha256 4cc1887e02d829633385c07582279a5712fbcd5e1b046eade01efba376237093
//                   -> ISteamInput   @ "SteamInput007"
// The action-data structs below are byte-packed (`#pragma pack(1)`) because
// the SDK packs them; getting that wrong corrupts every returned value.
//
// The harness never calls into anything undocumented: every function used
// here is part of the public Steamworks API surface.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "advapi32.lib")
  #endif
  using LibHandle = HMODULE;
  static LibHandle lib_open(const char* path) { return LoadLibraryA(path); }
  static void* lib_sym(LibHandle lib, const char* name) { return (void*)GetProcAddress(lib, name); }
  static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
  #include <dlfcn.h>
  #include <unistd.h>
  using LibHandle = void*;
  static LibHandle lib_open(const char* path) { return dlopen(path, RTLD_NOW); }
  static void* lib_sym(LibHandle lib, const char* name) { return dlsym(lib, name); }
  static void sleep_ms(unsigned ms) { usleep(ms * 1000); }
#endif

// ---------------------------------------------------------------------------
// Public SDK types (steamclientpublic.h / isteaminput.h, SDK 1.65)
// ---------------------------------------------------------------------------

using HSteamPipe = int32_t;
using HSteamUser = int32_t;

enum : int32_t { k_EAccountTypeIndividual = 1 };

using InputHandle_t = uint64_t;
using InputActionSetHandle_t = uint64_t;
using InputDigitalActionHandle_t = uint64_t;
using InputAnalogActionHandle_t = uint64_t;

constexpr int STEAM_INPUT_MAX_COUNT = 16;

enum EInputSourceMode {
  k_EInputSourceMode_None = 0,
  k_EInputSourceMode_Dpad,
  k_EInputSourceMode_Buttons,
  k_EInputSourceMode_FourButtons,
  k_EInputSourceMode_AbsoluteMouse,
  k_EInputSourceMode_RelativeMouse,
  k_EInputSourceMode_JoystickMove,
  k_EInputSourceMode_JoystickMouse,
  k_EInputSourceMode_JoystickCamera,
  k_EInputSourceMode_ScrollWheel,
  k_EInputSourceMode_Trigger,
  k_EInputSourceMode_TouchMenu,
  k_EInputSourceMode_MouseJoystick,
  k_EInputSourceMode_MouseRegion,
  k_EInputSourceMode_RadialMenu,
  k_EInputSourceMode_SingleButton,
  k_EInputSourceMode_Switches
};

enum ESteamInputType {
  k_ESteamInputType_Unknown = 0,
  k_ESteamInputType_SteamController,
  k_ESteamInputType_XBox360Controller,
  k_ESteamInputType_XBoxOneController,
  k_ESteamInputType_GenericGamepad,
  k_ESteamInputType_PS4Controller,
  k_ESteamInputType_AppleMFiController,
  k_ESteamInputType_AndroidController,
  k_ESteamInputType_SwitchJoyConPair,
  k_ESteamInputType_SwitchJoyConSingle,
  k_ESteamInputType_SwitchProController,
  k_ESteamInputType_MobileTouch,
  k_ESteamInputType_PS3Controller,
  k_ESteamInputType_PS5Controller,
  k_ESteamInputType_SteamDeckController,
  k_ESteamInputType_SteamOSHandheld,
  k_ESteamInputType_Switch2ProController,
  k_ESteamInputType_SteamController2026,
  k_ESteamInputType_SteamFrameControllerPair,
};

enum : uint32_t { k_ESteamInputLEDFlag_SetColor = 0 };

// EInputActionOrigin / EXboxOrigin / ESteamInputGlyphSize are opaque 32-bit
// tokens here; the engine's own GetStringForActionOrigin names them.
using EInputActionOrigin = int32_t;

#pragma pack(push, 1)

struct InputAnalogActionData_t {
  int32_t eMode;      // EInputSourceMode
  float x, y;
  bool bActive;
};

struct InputDigitalActionData_t {
  bool bState;
  bool bActive;
};

struct InputMotionData_t {
  float rotQuatX, rotQuatY, rotQuatZ, rotQuatW;
  float posAccelX, posAccelY, posAccelZ;
  float rotVelX, rotVelY, rotVelZ;
};

// Byte-compatible with the SDK's ScePadTriggerEffectParam
// (1-byte mode + two 48-byte per-trigger parameter blocks).
struct DualSenseTriggerEffectParam {
  uint8_t mode;  // 0=off 1=feedback 2=weapon 3=vibration
  struct Param {
    uint8_t position;
    uint8_t strength;    // feedback / weapon
    uint8_t amplitude;   // vibration
    uint8_t frequency;   // vibration, Hz
    uint8_t pad[44];
  } trigger[2];
};

#pragma pack(pop)

static_assert(sizeof(InputAnalogActionData_t) == 13, "packed layout mismatch");
static_assert(sizeof(InputDigitalActionData_t) == 2, "packed layout mismatch");
static_assert(sizeof(InputMotionData_t) == 40, "packed layout mismatch");
static_assert(sizeof(DualSenseTriggerEffectParam) == 97, "packed layout mismatch");

// ---------------------------------------------------------------------------
// Interface declarations — vtable slot order is load-bearing.
// ISteamClient ("SteamClient023"): 38 slots, GetISteamInput at slot 34.
// ISteamInput ("SteamInput007"): 48 slots.
// ---------------------------------------------------------------------------

struct ISteamClient {
  virtual HSteamPipe CreateSteamPipe() = 0;                                          // 0
  virtual bool BReleaseSteamPipe(HSteamPipe) = 0;                                    // 1
  virtual HSteamUser ConnectToGlobalUser(HSteamPipe) = 0;                            // 2
  virtual HSteamUser CreateLocalUser(HSteamPipe*, int32_t eAccountType) = 0;         // 3
  virtual void ReleaseUser(HSteamPipe, HSteamUser) = 0;                              // 4
  virtual void* GetISteamUser(HSteamUser, HSteamPipe, const char*) = 0;              // 5
  virtual void* GetISteamGameServer(HSteamUser, HSteamPipe, const char*) = 0;       // 6
  virtual void SetLocalIPBinding(const void* unIP, uint16_t) = 0;                    // 7
  virtual void* GetISteamFriends(HSteamUser, HSteamPipe, const char*) = 0;           // 8
  virtual void* GetISteamUtils(HSteamPipe, const char*) = 0;                         // 9
  virtual void* GetISteamMatchmaking(HSteamUser, HSteamPipe, const char*) = 0;      // 10
  virtual void* GetISteamMatchmakingServers(HSteamUser, HSteamPipe, const char*) = 0; // 11
  virtual void* GetISteamGenericInterface(HSteamUser, HSteamPipe, const char*) = 0; // 12
  virtual void* GetISteamUserStats(HSteamUser, HSteamPipe, const char*) = 0;        // 13
  virtual void* GetISteamGameServerStats(HSteamUser, HSteamPipe, const char*) = 0;  // 14
  virtual void* GetISteamApps(HSteamUser, HSteamPipe, const char*) = 0;             // 15
  virtual void* GetISteamNetworking(HSteamUser, HSteamPipe, const char*) = 0;       // 16
  virtual void* GetISteamRemoteStorage(HSteamUser, HSteamPipe, const char*) = 0;    // 17
  virtual void* GetISteamScreenshots(HSteamUser, HSteamPipe, const char*) = 0;      // 18
  virtual void RunFrame() = 0;                                                       // 19
  virtual uint32_t GetIPCCallCount() = 0;                                            // 20
  virtual void SetWarningMessageHook(void (*pfn)(int, const char*)) = 0;             // 21
  virtual bool BShutdownIfAllPipesClosed() = 0;                                      // 22
  virtual void* GetISteamHTTP(HSteamUser, HSteamPipe, const char*) = 0;             // 23
  virtual void* GetISteamController(HSteamUser, HSteamPipe, const char*) = 0;       // 24
  virtual void* GetISteamUGC(HSteamUser, HSteamPipe, const char*) = 0;              // 25
  virtual void* GetISteamMusic(HSteamUser, HSteamPipe, const char*) = 0;            // 26
  virtual void* GetISteamHTMLSurface(HSteamUser, HSteamPipe, const char*) = 0;      // 27
  virtual void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void (*)()) = 0;     // 28
  virtual void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void (*)()) = 0;  // 29
  virtual void Set_SteamAPI_CCheckCallbackRegisteredInProcess(void* fn) = 0;        // 30
  virtual void* GetISteamInventory(HSteamUser, HSteamPipe, const char*) = 0;        // 31
  virtual void* GetISteamVideo(HSteamUser, HSteamPipe, const char*) = 0;            // 32
  virtual void* GetISteamParentalSettings(HSteamUser, HSteamPipe, const char*) = 0; // 33
  virtual void* GetISteamInput(HSteamUser, HSteamPipe, const char* pchVersion) = 0; // 34
  virtual void* GetISteamParties(HSteamUser, HSteamPipe, const char*) = 0;          // 35
  virtual void* GetISteamRemotePlay(HSteamUser, HSteamPipe, const char*) = 0;       // 36
  virtual void DestroyAllInterfaces() = 0;                                          // 37
};

struct ISteamInput {
  virtual bool Init(bool bExplicitlyCallRunFrame) = 0;                               // 0
  virtual bool Shutdown() = 0;                                                       // 1
  virtual bool SetInputActionManifestFilePath(const char* pchAbsolutePath) = 0;      // 2
  virtual void RunFrame(bool bReservedValue) = 0;                                    // 3
  virtual bool BWaitForData(bool bWaitForever, uint32_t unTimeout) = 0;              // 4
  virtual bool BNewDataAvailable() = 0;                                              // 5
  virtual int GetConnectedControllers(InputHandle_t* handlesOut) = 0;                // 6
  virtual void EnableDeviceCallbacks() = 0;                                          // 7
  virtual void EnableActionEventCallbacks(void* pCallback) = 0;                      // 8
  virtual InputActionSetHandle_t GetActionSetHandle(const char* pszName) = 0;        // 9
  virtual void ActivateActionSet(InputHandle_t, InputActionSetHandle_t) = 0;         // 10
  virtual InputActionSetHandle_t GetCurrentActionSet(InputHandle_t) = 0;             // 11
  virtual void ActivateActionSetLayer(InputHandle_t, InputActionSetHandle_t) = 0;    // 12
  virtual void DeactivateActionSetLayer(InputHandle_t, InputActionSetHandle_t) = 0;  // 13
  virtual void DeactivateAllActionSetLayers(InputHandle_t) = 0;                      // 14
  virtual int GetActiveActionSetLayers(InputHandle_t, InputActionSetHandle_t*) = 0;  // 15
  virtual InputDigitalActionHandle_t GetDigitalActionHandle(const char*) = 0;        // 16
  virtual InputDigitalActionData_t GetDigitalActionData(InputHandle_t, InputDigitalActionHandle_t) = 0; // 17
  virtual int GetDigitalActionOrigins(InputHandle_t, InputActionSetHandle_t, InputDigitalActionHandle_t, EInputActionOrigin*) = 0; // 18
  virtual const char* GetStringForDigitalActionName(InputDigitalActionHandle_t) = 0; // 19
  virtual InputAnalogActionHandle_t GetAnalogActionHandle(const char*) = 0;          // 20
  virtual InputAnalogActionData_t GetAnalogActionData(InputHandle_t, InputAnalogActionHandle_t) = 0; // 21
  virtual int GetAnalogActionOrigins(InputHandle_t, InputActionSetHandle_t, InputAnalogActionHandle_t, EInputActionOrigin*) = 0; // 22
  virtual const char* GetGlyphPNGForActionOrigin(EInputActionOrigin, int32_t eSize, uint32_t unFlags) = 0; // 23
  virtual const char* GetGlyphSVGForActionOrigin(EInputActionOrigin, uint32_t) = 0;  // 24
  virtual const char* GetGlyphForActionOrigin_Legacy(EInputActionOrigin) = 0;        // 25
  virtual const char* GetStringForActionOrigin(EInputActionOrigin) = 0;              // 26
  virtual const char* GetStringForAnalogActionName(InputAnalogActionHandle_t) = 0;   // 27
  virtual void StopAnalogActionMomentum(InputHandle_t, InputAnalogActionHandle_t) = 0; // 28
  virtual InputMotionData_t GetMotionData(InputHandle_t) = 0;                        // 29
  virtual void TriggerVibration(InputHandle_t, uint16_t usLeft, uint16_t usRight) = 0; // 30
  virtual void TriggerVibrationExtended(InputHandle_t, uint16_t, uint16_t, uint16_t, uint16_t) = 0; // 31
  virtual void TriggerSimpleHapticEvent(InputHandle_t, int32_t eLocation, uint8_t, char, uint8_t, char) = 0; // 32
  virtual void SetLEDColor(InputHandle_t, uint8_t r, uint8_t g, uint8_t b, uint32_t nFlags) = 0; // 33
  virtual void Legacy_TriggerHapticPulse(InputHandle_t, int32_t ePad, uint16_t usMicroSec) = 0; // 34
  virtual void Legacy_TriggerRepeatedHapticPulse(InputHandle_t, int32_t ePad, uint16_t, uint16_t, uint16_t, uint32_t) = 0; // 35
  virtual bool ShowBindingPanel(InputHandle_t) = 0;                                  // 36
  virtual int32_t GetInputTypeForHandle(InputHandle_t) = 0;                          // 37
  virtual InputHandle_t GetControllerForGamepadIndex(int nIndex) = 0;                // 38
  virtual int GetGamepadIndexForController(InputHandle_t) = 0;                       // 39
  virtual const char* GetStringForXboxOrigin(int32_t) = 0;                           // 40
  virtual const char* GetGlyphForXboxOrigin(int32_t) = 0;                            // 41
  virtual EInputActionOrigin GetActionOriginFromXboxOrigin(InputHandle_t, int32_t) = 0; // 42
  virtual EInputActionOrigin TranslateActionOrigin(int32_t eDestType, EInputActionOrigin) = 0; // 43
  virtual bool GetDeviceBindingRevision(InputHandle_t, int* pMajor, int* pMinor) = 0; // 44
  virtual uint32_t GetRemotePlaySessionID(InputHandle_t) = 0;                        // 45
  virtual uint16_t GetSessionInputConfigurationSettings() = 0;                       // 46
  virtual void SetDualSenseTriggerEffect(InputHandle_t, const DualSenseTriggerEffectParam* pParam) = 0; // 47
};

using CreateInterfaceFn = void* (*)(const char* pchVersion, int* pReturnCode);

// ---------------------------------------------------------------------------
// Steam installation discovery
// ---------------------------------------------------------------------------

static bool file_exists(const std::string& p) {
#ifdef _WIN32
  return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  return access(p.c_str(), F_OK) == 0;
#endif
}

static std::string join_path(const std::string& a, const char* b) {
#ifdef _WIN32
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

#ifdef _WIN32
static std::string registry_steam_path() {
  char buf[MAX_PATH] = {};
  DWORD size = sizeof(buf);
  HKEY keys[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
  for (HKEY k : keys) {
    if (RegGetValueA(k, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
      return std::string(buf);
    size = sizeof(buf);
  }
  return {};
}
#endif

// Returns the Steam root directory and fills lib_path with the steamclient
// library inside it.
static std::string find_steam(std::string& lib_path) {
  std::vector<std::string> roots;
  const char* env = getenv("STEAM_DIR");
  if (env) roots.push_back(env);
#ifdef _WIN32
  std::string reg = registry_steam_path();
  if (!reg.empty()) roots.push_back(reg);
  const char* pf86 = getenv("ProgramFiles(x86)");
  if (pf86) roots.push_back(std::string(pf86) + "\\Steam");
#else
  const char* home = getenv("HOME");
  if (home) {
    roots.push_back(std::string(home) + "/.local/share/Steam");
    roots.push_back(std::string(home) + "/.steam/steam");
    roots.push_back(std::string(home) + "/.steam/root");
  }
  roots.push_back("/usr/lib/steam");
#endif
  for (const auto& root : roots) {
    if (!file_exists(root)) continue;
#ifdef _WIN32
    std::string lib = join_path(root, "steamclient64.dll");
    if (file_exists(lib)) { lib_path = lib; return root; }
#else
    for (const char* sub : {"linux64", "steamrt64", "ubuntu12_64"}) {
      std::string lib = join_path(join_path(root, sub), "steamclient.so");
      if (file_exists(lib)) { lib_path = lib; return root; }
    }
#endif
  }
  return {};
}

static std::string absolute_path(const std::string& p) {
#ifdef _WIN32
  char buf[MAX_PATH] = {};
  if (_fullpath(buf, p.c_str(), MAX_PATH)) return buf;
  return p;
#else
  char buf[4096] = {};
  if (realpath(p.c_str(), buf)) return buf;
  return p;
#endif
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

static const char* g_client_iface = "SteamClient023";
static const char* g_input_iface = "SteamInput007";
static bool g_json = false;
static volatile bool g_stop = false;
// teardown modes for diagnosing steamclient shutdown races:
//   full    = Shutdown + ReleaseUser + BShutdownIfAllPipesClosed
//   basic   = Shutdown + ReleaseUser
//   release = ReleaseUser only
//   none    = skip teardown, _exit immediately
static int g_teardown = 2;

#ifndef _WIN32
#include <csignal>
static void on_sigint(int) { g_stop = true; }
#else
static BOOL WINAPI on_ctrlc(DWORD) { g_stop = true; return TRUE; }
#endif

static void warning_hook(int severity, const char* text) {
  fprintf(stderr, "[steam%s] %s\n", severity ? " WARNING" : "", text ? text : "");
}

static const char* input_type_name(int32_t t) {
  switch (t) {
    case k_ESteamInputType_Unknown: return "Unknown";
    case k_ESteamInputType_SteamController: return "SteamController";
    case k_ESteamInputType_XBox360Controller: return "XBox360";
    case k_ESteamInputType_XBoxOneController: return "XBoxOne";
    case k_ESteamInputType_GenericGamepad: return "GenericGamepad";
    case k_ESteamInputType_PS4Controller: return "PS4/DualShock4";
    case k_ESteamInputType_PS3Controller: return "PS3";
    case k_ESteamInputType_PS5Controller: return "PS5/DualSense";
    case k_ESteamInputType_SwitchProController: return "SwitchPro";
    case k_ESteamInputType_Switch2ProController: return "Switch2Pro";
    case k_ESteamInputType_SteamDeckController: return "SteamDeck";
    case k_ESteamInputType_SteamOSHandheld: return "SteamOSHandheld";
    case k_ESteamInputType_SteamController2026: return "SteamController2026";
    case k_ESteamInputType_SteamFrameControllerPair: return "SteamFramePair";
    default: return "Other";
  }
}

static const char* source_mode_name(int32_t m) {
  switch (m) {
    case k_EInputSourceMode_None: return "none";
    case k_EInputSourceMode_Dpad: return "dpad";
    case k_EInputSourceMode_Buttons: return "buttons";
    case k_EInputSourceMode_FourButtons: return "fourbuttons";
    case k_EInputSourceMode_AbsoluteMouse: return "absmouse";
    case k_EInputSourceMode_RelativeMouse: return "relmouse";
    case k_EInputSourceMode_JoystickMove: return "joystick_move";
    case k_EInputSourceMode_JoystickMouse: return "joystick_mouse";
    case k_EInputSourceMode_JoystickCamera: return "joystick_camera";
    case k_EInputSourceMode_ScrollWheel: return "scrollwheel";
    case k_EInputSourceMode_Trigger: return "trigger";
    case k_EInputSourceMode_TouchMenu: return "touchmenu";
    case k_EInputSourceMode_MouseJoystick: return "mousejoystick";
    case k_EInputSourceMode_MouseRegion: return "mouseregion";
    case k_EInputSourceMode_RadialMenu: return "radialmenu";
    case k_EInputSourceMode_SingleButton: return "singlebutton";
    case k_EInputSourceMode_Switches: return "switches";
    default: return "?";
  }
}

struct ActionRef {
  const char* name;
  bool analog;
  union {
    InputDigitalActionHandle_t digital;
    InputAnalogActionHandle_t analog_h;
  } handle = {};
  // last observed state
  bool last_b = false;
  float last_x = 0, last_y = 0;
};

static void usage() {
  fputs(
    "steaminput-harness — drive Steam Input standalone via steamclient\n"
    "\n"
    "Usage: steaminput-harness [options]\n"
    "\n"
    "  --steam-dir <dir>     Steam root (default: autodetect)\n"
    "  --manifest <path>     action manifest VDF to load\n"
    "  --set <name>          action set to activate (default: gameplay)\n"
    "  --list                enumerate controllers, print details, exit\n"
    "  --poll <seconds>      monitor action state; 0 = until Ctrl-C (default 10)\n"
    "  --motion              include motion data in monitor output\n"
    "  --json                JSONL output for scripting\n"
    "  --rumble <L,R>        set vibration motors 0-65535 and exit\n"
    "  --led <R,G,B>         set lightbar color 0-255 and exit\n"
    "  --trigger-effect <mode>,<pos>,<strength>   DualSense adaptive triggers\n"
    "                        mode: off|feedback|weapon|vibration\n"
    "  --client-iface <v>    ISteamClient version (default SteamClient023)\n"
    "  --input-iface <v>     ISteamInput version (default SteamInput007)\n"
    "  -q                    quiet: only events, no banner\n"
    "\n"
    "Exit codes: 0 ok, 1 runtime error, 2 setup error, 3 no controllers found\n",
    stderr);
}

int main(int argc, char** argv) {
  std::string steam_dir_arg, manifest, set_name = "gameplay";
  std::string rumble, led, trigger_effect;
  bool do_list = false, motion = false, quiet = false;
  double poll_seconds = -1;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](const char* what) -> const char* {
      if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", what); exit(2); }
      return argv[++i];
    };
    if (a == "--steam-dir") steam_dir_arg = need("--steam-dir");
    else if (a == "--manifest") manifest = need("--manifest");
    else if (a == "--set") set_name = need("--set");
    else if (a == "--list") do_list = true;
    else if (a == "--poll") poll_seconds = atof(need("--poll"));
    else if (a == "--motion") motion = true;
    else if (a == "--json") g_json = true;
    else if (a == "--rumble") rumble = need("--rumble");
    else if (a == "--led") led = need("--led");
    else if (a == "--trigger-effect") trigger_effect = need("--trigger-effect");
    else if (a == "--client-iface") g_client_iface = strdup(need("--client-iface"));
    else if (a == "--input-iface") g_input_iface = strdup(need("--input-iface"));
    else if (a == "--teardown") {
      const char* m = need("--teardown");
      g_teardown = !strcmp(m, "none") ? 0 : !strcmp(m, "basic") ? 1 : !strcmp(m, "release") ? 3 : 2;
    }
    else if (a == "-q") quiet = true;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else { fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
  }

  // ------------------------------------------------------------------ setup
  // Resolve user-supplied paths before chdir into the Steam root.
  if (!manifest.empty()) manifest = absolute_path(manifest);

  std::string lib_path;
  std::string steam_dir = !steam_dir_arg.empty() ? steam_dir_arg : find_steam(lib_path);
  if (steam_dir_arg.empty() && steam_dir.empty()) {
    fprintf(stderr, "error: no Steam installation found; pass --steam-dir\n");
    return 2;
  }
  if (lib_path.empty()) {
#ifdef _WIN32
    lib_path = join_path(steam_dir, "steamclient64.dll");
#else
    lib_path = join_path(steam_dir, "linux64/steamclient.so");
#endif
  }
  if (!file_exists(lib_path)) {
    fprintf(stderr, "error: %s not found\n", lib_path.c_str());
    return 2;
  }

  LibHandle lib = lib_open(lib_path.c_str());
  if (!lib) {
#ifdef _WIN32
    fprintf(stderr, "error: LoadLibrary failed (%lu)\n", GetLastError());
#else
    fprintf(stderr, "error: dlopen failed: %s\n", dlerror());
#endif
    return 2;
  }
  auto create_interface = (CreateInterfaceFn)lib_sym(lib, "CreateInterface");
  if (!create_interface) {
    fprintf(stderr, "error: steamclient has no CreateInterface export\n");
    return 2;
  }

  // The engine resolves assets (controller_base, crashhandler) relative to
  // the Steam root, so run from there like a game process would.
#ifdef _WIN32
  if (!SetCurrentDirectoryA(steam_dir.c_str()))
#else
  if (chdir(steam_dir.c_str()) != 0)
#endif
    fprintf(stderr, "warn: could not chdir into %s\n", steam_dir.c_str());

#ifndef _WIN32
  signal(SIGINT, on_sigint);
#else
  SetConsoleCtrlHandler(on_ctrlc, TRUE);
#endif
  setvbuf(stdout, nullptr, _IOLBF, 0);

  if (!quiet && !g_json)
    printf("steamclient : %s\nclient iface: %s\ninput iface : %s\n",
           lib_path.c_str(), g_client_iface, g_input_iface);

  // ------------------------------------------------------------- interfaces
  int rc = 0;
  auto client = (ISteamClient*)create_interface(g_client_iface, &rc);
  if (!client) {
    fprintf(stderr, "error: CreateInterface(%s) failed (rc=%d)\n", g_client_iface, rc);
    return 2;
  }
  client->SetWarningMessageHook(&warning_hook);

  HSteamPipe pipe = 0;
  HSteamUser user = client->CreateLocalUser(&pipe, k_EAccountTypeIndividual);
  if (!user || !pipe) {
    fprintf(stderr, "error: CreateLocalUser failed\n");
    return 2;
  }

  auto input = (ISteamInput*)client->GetISteamInput(user, pipe, g_input_iface);
  if (!input) {
    fprintf(stderr, "error: GetISteamInput(%s) returned null\n", g_input_iface);
    return 2;
  }

  if (!manifest.empty()) {
    if (!input->SetInputActionManifestFilePath(manifest.c_str())) {
      fprintf(stderr, "error: manifest rejected: %s\n", manifest.c_str());
      goto teardown_fail;
    }
    if (!quiet && !g_json) printf("manifest    : %s\n", manifest.c_str());
  }

  // We drive frames ourselves.
  if (!input->Init(true)) {
    fprintf(stderr, "error: ISteamInput::Init failed\n");
    goto teardown_fail;
  }

  // ------------------------------------------------------------ enumerate
  {
    InputHandle_t handles[STEAM_INPUT_MAX_COUNT] = {};
    int n = 0;
    // the device scanner thread may still be starting
    for (int attempt = 0; attempt < 30 && !g_stop; attempt++) {
      n = input->GetConnectedControllers(handles);
      if (n > 0 || attempt == 29) break;
      sleep_ms(100);
    }
    if (!quiet || g_json) {
      if (g_json)
        printf("{\"event\":\"enumeration\",\"count\":%d}\n", n);
      else if (!quiet)
        printf("controllers  : %d\n", n);
    }
    if (n <= 0) {
      fprintf(stderr, "no controllers visible to the engine%s\n",
              manifest.empty() ? "" : " (note: some builds require a manifest before devices attach)");
      switch (g_teardown) {
        case 0: _exit(3);
        case 1:
          input->Shutdown();
          client->ReleaseUser(pipe, user);
          break;
        case 3:
          client->ReleaseUser(pipe, user);
          break;
        default:
          input->Shutdown();
          client->ReleaseUser(pipe, user);
          client->BShutdownIfAllPipesClosed();
      }
      _exit(3);
    }

    for (int d = 0; d < n; d++) {
      int32_t type = input->GetInputTypeForHandle(handles[d]);
      int major = -1, minor = -1;
      input->GetDeviceBindingRevision(handles[d], &major, &minor);
      if (g_json)
        printf("{\"event\":\"device\",\"handle\":%llu,\"type\":\"%s\",\"binding_rev\":\"%d.%d\"}\n",
               (unsigned long long)handles[d], input_type_name(type), major, minor);
      else if (!quiet)
        printf("  [%d] handle %llu  %s  binding %d.%d\n", d,
               (unsigned long long)handles[d], input_type_name(type), major, minor);
    }

    if (do_list) goto teardown_ok;

    // ------------------------------------------------------- one-shot output
    if (!rumble.empty() || !led.empty() || !trigger_effect.empty()) {
      unsigned l = 0, r = 0, lr = 0, lg = 0, lb = 0;
      if (!rumble.empty() && sscanf(rumble.c_str(), "%u,%u", &l, &r) != 2) {
        fprintf(stderr, "--rumble expects <L,R>\n"); goto teardown_fail;
      }
      if (!led.empty() && sscanf(led.c_str(), "%u,%u,%u", &lr, &lg, &lb) != 3) {
        fprintf(stderr, "--led expects <R,G,B>\n"); goto teardown_fail;
      }
      if (!trigger_effect.empty()) {
        char mode_s[32] = {};
        unsigned pos = 0, strength = 0;
        if (sscanf(trigger_effect.c_str(), "%31[^,],%u,%u", mode_s, &pos, &strength) < 1) {
          fprintf(stderr, "--trigger-effect expects <mode>[,<pos>,<strength>]\n"); goto teardown_fail;
        }
        DualSenseTriggerEffectParam p = {};
        if (!strcmp(mode_s, "off")) p.mode = 0;
        else if (!strcmp(mode_s, "feedback")) p.mode = 1;
        else if (!strcmp(mode_s, "weapon")) p.mode = 2;
        else if (!strcmp(mode_s, "vibration")) p.mode = 3;
        else { fprintf(stderr, "unknown trigger mode '%s'\n", mode_s); goto teardown_fail; }
        for (int t = 0; t < 2; t++) {
          p.trigger[t].position = (uint8_t)pos;
          p.trigger[t].strength = (uint8_t)strength;
          p.trigger[t].amplitude = (uint8_t)strength;
          p.trigger[t].frequency = 30;
        }
        for (int d2 = 0; d2 < n; d2++)
          input->SetDualSenseTriggerEffect(handles[d2], &p);
      }
      if (!rumble.empty())
        for (int d2 = 0; d2 < n; d2++)
          input->TriggerVibration(handles[d2], (uint16_t)l, (uint16_t)r);
      if (!led.empty())
        for (int d2 = 0; d2 < n; d2++)
          input->SetLEDColor(handles[d2], (uint8_t)lr, (uint8_t)lg, (uint8_t)lb, k_ESteamInputLEDFlag_SetColor);
      input->RunFrame(true);
      sleep_ms(500);
      goto teardown_ok;
    }

    // ------------------------------------------------------------ monitoring
    if (manifest.empty()) {
      if (!quiet && !g_json)
        printf("controllers present; pass --manifest <vdf> to monitor actions\n");
      goto teardown_ok;
    }

    InputActionSetHandle_t set = input->GetActionSetHandle(set_name.c_str());
    if (!set) {
      fprintf(stderr, "error: action set '%s' not found in manifest\n", set_name.c_str());
      goto teardown_fail;
    }
    for (int d2 = 0; d2 < n; d2++) input->ActivateActionSet(handles[d2], set);

    std::vector<ActionRef> actions = {
      {"left_stick", true}, {"right_stick", true},
      {"left_trigger", true}, {"right_trigger", true},
      {"dpad_north", false}, {"dpad_south", false}, {"dpad_west", false}, {"dpad_east", false},
      {"south", false}, {"east", false}, {"west", false}, {"north", false},
      {"left_shoulder", false}, {"right_shoulder", false},
      {"left_stick_click", false}, {"right_stick_click", false},
      {"select", false}, {"start", false}, {"guide", false},
      {"capture", false}, {"touchpad", false},
      {"left_trigger_click", false}, {"right_trigger_click", false},
    };
    for (auto& a2 : actions) {
      if (a2.analog) a2.handle.analog_h = input->GetAnalogActionHandle(a2.name);
      else a2.handle.digital = input->GetDigitalActionHandle(a2.name);
    }

    if (poll_seconds < 0) poll_seconds = 10;
    unsigned long long frames = 0;
    time_t t_end = time(nullptr) + (time_t)poll_seconds;

    while (!g_stop && (poll_seconds == 0 || time(nullptr) < t_end)) {
      input->BWaitForData(false, 100);
      input->RunFrame(true);
      frames++;

      for (int d2 = 0; d2 < n; d2++) {
        for (auto& a2 : actions) {
          if (a2.analog) {
            if (!a2.handle.analog_h) continue;
            InputAnalogActionData_t v = input->GetAnalogActionData(handles[d2], a2.handle.analog_h);
            float dx = v.x - a2.last_x, dy = v.y - a2.last_y;
            if (v.bActive && (dx * dx + dy * dy) > 0.0025f) {
              a2.last_x = v.x; a2.last_y = v.y;
              if (g_json)
                printf("{\"event\":\"analog\",\"action\":\"%s\",\"x\":%.4f,\"y\":%.4f,\"mode\":\"%s\"}\n",
                       a2.name, v.x, v.y, source_mode_name(v.eMode));
              else
                printf("%-18s %6.3f %6.3f  (%s)\n", a2.name, v.x, v.y, source_mode_name(v.eMode));
            }
          } else {
            if (!a2.handle.digital) continue;
            InputDigitalActionData_t v = input->GetDigitalActionData(handles[d2], a2.handle.digital);
            if (v.bActive && v.bState != a2.last_b) {
              a2.last_b = v.bState;
              EInputActionOrigin origins[8] = {};
              int no = input->GetDigitalActionOrigins(handles[d2], set, a2.handle.digital, origins);
              const char* origin = no > 0 && input->GetStringForActionOrigin(origins[0])
                                     ? input->GetStringForActionOrigin(origins[0]) : "?";
              if (g_json)
                printf("{\"event\":\"digital\",\"action\":\"%s\",\"state\":%s,\"origin\":\"%s\"}\n",
                       a2.name, v.bState ? "true" : "false", origin);
              else
                printf("%-18s %s  via %s\n", a2.name, v.bState ? "down" : "up  ", origin);
            }
          }
        }
        if (motion) {
          InputMotionData_t m = input->GetMotionData(handles[d2]);
          if (g_json)
            printf("{\"event\":\"motion\",\"quat\":[%.4f,%.4f,%.4f,%.4f],\"accel\":[%.1f,%.1f,%.1f],\"gyro\":[%.1f,%.1f,%.1f]}\n",
                   m.rotQuatX, m.rotQuatY, m.rotQuatZ, m.rotQuatW,
                   m.posAccelX, m.posAccelY, m.posAccelZ, m.rotVelX, m.rotVelY, m.rotVelZ);
          else
            printf("motion  q(%.2f %.2f %.2f %.2f) a(%5.0f %5.0f %5.0f) g(%6.0f %6.0f %6.0f)\n",
                   m.rotQuatX, m.rotQuatY, m.rotQuatZ, m.rotQuatW,
                   m.posAccelX, m.posAccelY, m.posAccelZ, m.rotVelX, m.rotVelY, m.rotVelZ);
        }
      }
    }
    if (!quiet && !g_json) printf("polled %llu frames\n", frames);
  }

teardown_ok:
  switch (g_teardown) {
    case 0:
      _exit(0);
    case 1:
      input->Shutdown();
      client->ReleaseUser(pipe, user);
      break;
    default:
      input->Shutdown();
      client->ReleaseUser(pipe, user);
      client->BShutdownIfAllPipesClosed();
  }
  // Deliberately no dlclose/FreeLibrary: steamclient keeps worker threads
  // alive past interface shutdown and unloading it races them.
  _exit(0);

teardown_fail:
  input->Shutdown();
  client->ReleaseUser(pipe, user);
  client->BShutdownIfAllPipesClosed();
  _exit(1);
}
