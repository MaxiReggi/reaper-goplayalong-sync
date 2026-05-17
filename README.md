# REAPER GoPlayAlong Sync

A REAPER plugin that keeps GoPlayAlong 4 and REAPER in perfect playback sync.
Play along with Guitar Pro tabs in GoPlayAlong while hearing your full REAPER
project (VST effects, backing tracks, etc.) — both applications stay locked
in sync automatically.

Inspired by [reaper-guitar-pro-sync](https://github.com/tnt-coders/reaper-guitar-pro-sync).

---

## How it works

The plugin runs as a REAPER timer (~30 Hz). Each tick it reads GoPlayAlong's
internal process memory to obtain the current playback position, play/pause
state, loop boundaries, and playback speed, then issues the corresponding
commands to REAPER via its plugin API.

Because it reads memory directly (no official API exists), the memory offsets
must be found once per GoPlayAlong version using a memory scanner.

---

## Status

| Component | Status |
|---|---|
| REAPER plugin infrastructure | Done |
| Sync engine (desync detection, latency compensation) | Done |
| Memory reading infrastructure (`ProcessReader`) | Done |
| **GoPlayAlong memory offsets** | **TODO — requires Windows** |

---

## Building

### Prerequisites

- CMake >= 3.15
- A C++20 compiler
  - **Windows**: MSVC (Visual Studio 2022) or MinGW
  - **Linux/macOS**: GCC or Clang (for development; the final plugin must be built for Windows)

### Steps

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build
```

The install step copies the `.dll` to `%APPDATA%\REAPER\UserPlugins` on Windows
(or the equivalent path on other platforms).

---

## Finding memory offsets (Windows — one-time setup)

This is the only step that requires Windows and GoPlayAlong 4 to be running.
Once the offsets are found, they are hardcoded into `src/goplayalong.cpp` and
the plugin works for everyone on the same GoPlayAlong version.

### Tools needed

- [scanmem](https://github.com/scanmem/scanmem) (Linux/Wine) **or**
  [CheatEngine](https://cheatengine.org/) (Windows)

### Step 1 — Identify the process and module name

Open Task Manager while GoPlayAlong 4 is running and note the exact `.exe`
filename shown in the Details tab (e.g. `GoPlayAlong.exe`).

Update the constants at the top of `src/goplayalong.cpp`:

```cpp
static constexpr wchar_t PROCESS_NAME[] = L"GoPlayAlong.exe";  // exact name from Task Manager
static constexpr wchar_t MODULE_NAME[]  = L"GoPlayAlong.exe";  // same if no separate DLL
```

If GoPlayAlong loads a separate DLL that likely contains the playback engine,
use that DLL name as `MODULE_NAME` instead.

### Step 2 — Find the playback position address

1. Open GoPlayAlong, load any song, play and then **pause at a known position**
   (e.g. exactly at the 10-second mark — use the time display).
2. In scanmem/CheatEngine, attach to the GoPlayAlong process.
3. Search for the value `10` as a 4-byte integer (samples at 44100 Hz = 441000)
   **and** as a `float`/`double` (10.0).
4. Resume playback, pause at a different position, then filter the results to
   values that match the new position.
5. Repeat until you have 1–3 candidate addresses.
6. In CheatEngine: right-click the address → "Find out what accesses this address"
   to reveal the pointer chain back to the module base.
   In scanmem: use `list` and manually trace back with `ptrscan`.

Record the **module offset** and **pointer chain** (list of hex offsets).

### Step 3 — Find the remaining values

Repeat Step 2 for each of the following, noting their pointer chains:

| Value | How to find it |
|---|---|
| `time_selection_start` | Set a loop start point, search for its position |
| `time_selection_end` | Set a loop end point, search for its position |
| `play_rate` | Set speed to 75% (value ~0.75 as float), then 100% |
| `play_state` | Search for value that is 1 while playing and 0 while stopped |
| `loop_state` | Toggle loop on/off, search for 0/1 change |
| `count_in_state` | Enable count-in, search for 0/1 change (may not exist) |

### Step 4 — Update goplayalong.cpp

Replace the placeholder offsets in `src/goplayalong.cpp`:

```cpp
// Example — replace with your actual values:
static constexpr int MODULE_OFFSET = 0x00AB1234;

// ReadMemoryAddress takes the module offset + a pointer chain:
const int raw_position = reader.ReadMemoryAddress<int>(
    MODULE_OFFSET, { 0x18, 0xA0, 0x38, 0x1D8, 0x0 });
```

Also update the position unit conversion if needed:
- **Samples (int)**: `static_cast<double>(raw_position) / SAMPLE_RATE` (keep as-is)
- **Seconds (double)**: read as `double` and use directly
- **Milliseconds (int)**: divide by `1000.0`

### Step 5 — Test

Build the plugin, copy the `.dll` to REAPER's `UserPlugins` folder, restart
REAPER, and enable *"TNT: Toggle GoPlayAlong sync"* from the Action List.
Open GoPlayAlong with a song loaded. Press play in GoPlayAlong — REAPER should
follow immediately.

---

## Installation (after offsets are found and plugin is built)

1. Build for Windows (see Building above).
2. Copy `reaper_GoPlayAlongSync-x86_64.dll` to:
   `%APPDATA%\REAPER\UserPlugins\`
3. Restart REAPER.
4. Open the Action List (`?` key) and search for "GoPlayAlong".
5. Run *"TNT: Toggle GoPlayAlong sync"* to enable/disable the sync.

---

## Troubleshooting

**REAPER console shows "Process not found"**
Make sure GoPlayAlong is running before enabling the sync action in REAPER.

**REAPER console shows "Module not found"**
The `MODULE_NAME` constant may be wrong. Open Process Explorer, attach to the
GoPlayAlong process, and look at the Modules tab for the correct DLL name.

**Playback is off by a constant amount**
Adjust `LATENCY_COMPENSATION` in `src/plugin.cpp` (default: 50 ms).

**Offsets stop working after a GoPlayAlong update**
The memory layout may have changed. Repeat the offset-finding steps and add a
new version branch in `GoPlayAlong::Impl::ReadProcessMemory()`.
