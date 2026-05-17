#include "goplayalong.h"

#include "process_reader.h"
#include "wstring_utils.h"

#include <format>
#include <stdexcept>
#include <utility>

// =============================================================================
// HOW TO COMPLETE THIS FILE (requires Windows + GoPlayAlong 4 running)
// =============================================================================
//
// This file reads GoPlayAlong's internal memory to extract playback state.
// The memory offsets below are PLACEHOLDERS and must be found using scanmem
// or CheatEngine on Windows/Wine with GoPlayAlong 4 running.
//
// STEP 1 — Find the process name and module
//   Open Windows Task Manager (or Process Explorer) while GoPlayAlong is running.
//   Note the exact .exe name (e.g. "GoPlayAlong.exe", "GoPlayAlong 4.exe").
//   If it is an Adobe AIR app, the main module IS the .exe itself — use the
//   same name for both PROCESS_NAME and MODULE_NAME below.
//   Update the constants PROCESS_NAME and MODULE_NAME accordingly.
//
// STEP 2 — Find the playback position in memory
//   Using scanmem (Linux/Wine) or CheatEngine (Windows):
//     a) Play a song, pause at a known position (e.g. exactly 10 seconds).
//     b) Search for the value 10.0 as a float or double.
//        Also try searching for the value in samples (10 * 44100 = 441000)
//        as an int, in case GoPlayAlong stores position in samples like Guitar Pro.
//     c) Resume playback, pause again at a new position. Filter the results.
//     d) Repeat until you narrow it down to 1-3 addresses.
//     e) Use "Find what accesses this address" to trace back to the base pointer.
//        CheatEngine will show you the pointer chain (e.g. module+0xABCD -> +0x18 -> +0x40).
//   Record the MODULE_OFFSET and POSITION_OFFSETS chain.
//
// STEP 3 — Find play state, loop state, and play rate
//   Repeat the same process for:
//     - play_state:  search for a boolean/DWORD that is 1 when playing, 0 when stopped
//     - loop_state:  search for a boolean/DWORD that is 1 when loop is enabled
//     - play_rate:   search for a float near 1.0 (100% speed), change speed, filter
//   These are bit flags in Guitar Pro — they may be plain bools in GoPlayAlong.
//   Adjust the state-reading code below as needed.
//
// STEP 4 — Determine position units
//   Check whether the position value is:
//     - Seconds (double/float): use it directly
//     - Samples (int at 44100 Hz): divide by SAMPLE_RATE
//     - Milliseconds (int): divide by 1000.0
//   Update the state-building section at the bottom of ReadProcessMemory().
//
// STEP 5 — Verify version handling
//   Call process_reader.GetProcessVersion() and check the returned string.
//   If GoPlayAlong has a single stable memory layout across versions, you can
//   skip version-specific offsets. Otherwise, add cases as in Guitar Pro.
//
// =============================================================================

namespace tnt {

// TODO(windows): Update these after identifying the process in Task Manager.
static constexpr wchar_t PROCESS_NAME[] = L"GoPlayAlong.exe";
static constexpr wchar_t MODULE_NAME[]  = L"GoPlayAlong.exe"; // Same as process if no separate DLL

// TODO(windows): Update after finding the module base offset with scanmem/CheatEngine.
static constexpr int MODULE_OFFSET = 0x00000000;

// TODO(windows): Replace these placeholder offset chains with the real ones.
// Format: each list follows the pointer chain as exported by CheatEngine.
// Example from Guitar Pro: { 0x18, 0xA0, 0x38, 0x1A8, 0x20, 0x1D8, 0x0 }
//
// If GoPlayAlong stores position as a double/float (seconds), use that directly.
// If it stores position as samples (int), divide by SAMPLE_RATE below.
static constexpr int POSITION_OFFSETS[]              = {0x0}; // TODO(windows)
static constexpr int TIME_SELECTION_START_OFFSETS[]  = {0x0}; // TODO(windows)
static constexpr int TIME_SELECTION_END_OFFSETS[]    = {0x0}; // TODO(windows)
static constexpr int PLAY_RATE_OFFSETS[]             = {0x0}; // TODO(windows)
static constexpr int PLAY_STATE_OFFSETS[]            = {0x0}; // TODO(windows)
static constexpr int LOOP_STATE_OFFSETS[]            = {0x0}; // TODO(windows)
static constexpr int COUNT_IN_STATE_OFFSETS[]        = {0x0}; // TODO(windows) - may not exist in GoPlayAlong

// TODO(windows): Verify whether GoPlayAlong stores position in samples or seconds.
// Guitar Pro uses 44100 Hz. Set to 1 if position is already in seconds.
static constexpr int SAMPLE_RATE = 44100;

// TODO(windows): Adjust flag bit positions if GoPlayAlong uses bit fields like Guitar Pro.
// If it uses plain bool values, change the state extraction below accordingly.
static constexpr int PLAY_STATE_FLAG_BIT  = 8;
static constexpr int LOOP_STATE_FLAG_BIT  = 8;
static constexpr int COUNT_IN_FLAG_BIT    = 8;

struct GoPlayAlong::Impl final
{
    GoPlayAlongState ReadProcessMemory()
    {
        const ProcessReader reader(PROCESS_NAME, MODULE_NAME);

        // Optional: version check — add cases here if offsets differ between versions.
        // const auto version = reader.GetProcessVersion();

        // Read raw values from process memory
        // TODO(windows): Change the type parameter <int> to match what GoPlayAlong
        // actually stores (e.g. <double>, <float>, or <int> for samples).
        const int raw_position = reader.ReadMemoryAddress<int>(
            MODULE_OFFSET, {POSITION_OFFSETS[0]});

        int raw_sel_start = reader.ReadMemoryAddress<int>(
            MODULE_OFFSET, {TIME_SELECTION_START_OFFSETS[0]});

        int raw_sel_end = reader.ReadMemoryAddress<int>(
            MODULE_OFFSET, {TIME_SELECTION_END_OFFSETS[0]});

        const float raw_play_rate = reader.ReadMemoryAddress<float>(
            MODULE_OFFSET, {PLAY_RATE_OFFSETS[0]});

        // TODO(windows): Adjust these reads based on whether GoPlayAlong uses
        // bit flags (DWORD) or plain booleans (bool/BYTE) for state values.
        const DWORD raw_play_state = reader.ReadMemoryAddress<DWORD>(
            MODULE_OFFSET, {PLAY_STATE_OFFSETS[0]});

        const DWORD raw_loop_state = reader.ReadMemoryAddress<DWORD>(
            MODULE_OFFSET, {LOOP_STATE_OFFSETS[0]});

        const DWORD raw_count_in = reader.ReadMemoryAddress<DWORD>(
            MODULE_OFFSET, {COUNT_IN_STATE_OFFSETS[0]});

        // Ensure time selection start is always before end
        if (raw_sel_start > raw_sel_end)
        {
            std::swap(raw_sel_start, raw_sel_end);
        }

        GoPlayAlongState state{};

        // TODO(windows): Adjust the position conversion to match the unit used by GoPlayAlong.
        // If position is stored in samples:   divide by SAMPLE_RATE
        // If position is stored in seconds:   cast directly to double
        // If position is stored in ms:        divide by 1000.0
        state.play_position                  = static_cast<double>(raw_position) / SAMPLE_RATE;
        state.time_selection_start_position  = static_cast<double>(raw_sel_start) / SAMPLE_RATE;
        state.time_selection_end_position    = static_cast<double>(raw_sel_end)   / SAMPLE_RATE;
        state.play_rate                      = static_cast<double>(raw_play_rate);

        // TODO(windows): If GoPlayAlong uses plain booleans instead of bit flags,
        // replace these with: state.play_state = raw_play_state != 0;
        state.play_state     = raw_play_state  & (1U << PLAY_STATE_FLAG_BIT);
        state.loop_state     = raw_loop_state  & (1U << LOOP_STATE_FLAG_BIT);
        state.count_in_state = raw_count_in    & (1U << COUNT_IN_FLAG_BIT);

        return state;
    }
};

GoPlayAlong::GoPlayAlong()
    : m_impl(std::make_unique<Impl>())
{}

GoPlayAlong::~GoPlayAlong() = default;

GoPlayAlongState GoPlayAlong::ReadProcessMemory()
{
    return m_impl->ReadProcessMemory();
}

}
