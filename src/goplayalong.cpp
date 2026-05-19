#include "goplayalong.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

// =============================================================================
// Dynamic position discovery — no hardcoded pointer chains
//
// GoPlayAlong 4 allocates playback state in dynamic heap memory (Electron/V8),
// so static pointer chains don't survive process restarts. Instead, on each
// plugin activation we scan GoPlayAlong's memory for a double that advances
// at ~1.0 second per real second (normal playback rate). Once found, the
// address is cached for the session.
//
// What we know from reverse engineering:
//   - Process name:  "Go PlayAlong 4.exe"  (5 processes, pick largest RSS)
//   - Position type: double (seconds)
//   - Written by:    movsd [eax+03],xmm1   at Go PlayAlong 4.exe+0x33E6A23
//   - Module base:   0x00730000 (no ASLR on this app)
//
// Other state (play_state, loop, time_selection, play_rate) is not yet found.
// play_state is inferred from the position rate-of-change.
// =============================================================================

namespace tnt {

static constexpr wchar_t PROCESS_NAME[]          = L"Go PlayAlong 4.exe";
static constexpr double  MAX_DURATION            = 10800.0; // 3 hours in seconds
static constexpr double  MIN_POSITION            = 0.5;     // must be past first 0.5 s to scan
static constexpr int     CONFIRM_TICKS           = 5;       // consistent ticks needed to commit
static constexpr double  RATE_TOLERANCE          = 0.20;    // ±20% on the expected 1.0 s/s rate
static constexpr int     RESCAN_INTERVAL         = 150;     // filter ticks before giving up and re-scanning
static constexpr int     PAUSED_TICKS_THRESHOLD   = 10;      // ticks of no movement before declaring paused (~333ms)
static constexpr int     STALE_ADDRESS_THRESHOLD  = 90;      // ticks of continuous pause before forcing rediscovery (~3s)
static constexpr double  MIN_SCAN_INTERVAL_SEC    = 1.0;     // minimum seconds between scan restarts
// Max bytes read per timer tick during the scan phase. Keeps each tick under ~2 ms
// even on slow machines, so the REAPER main thread is never visibly blocked.
static constexpr SIZE_T  SCAN_BYTES_PER_TICK     = 16ULL * 1024 * 1024;

struct GoPlayAlong::Impl
{
    HANDLE    m_process       = nullptr;
    uintptr_t m_position_addr = 0;
    double    m_last_position = 0.0;
    bool      m_is_playing   = false;
    int       m_paused_ticks = 0;

    struct Region {
        uintptr_t base;
        SIZE_T    size;
    };

    struct Candidate {
        uintptr_t address;
        double    last_value;
        int       ticks;          // -1 = invalid, remove on next pass
        size_t    region_idx;
        size_t    region_offset;
    };

    // Incremental scan state
    bool   m_scan_done    = false; // true once all queued regions have been read
    bool   m_scan_started = false; // true after BeginScan() until scan completes or resets
    std::vector<MEMORY_BASIC_INFORMATION> m_scan_queue; // regions to read, populated by BeginScan()
    size_t m_scan_progress = 0;                         // next index into m_scan_queue
    std::chrono::steady_clock::time_point m_last_scan_time; // epoch → first scan always immediate

    // Filter / discovery state
    std::vector<Region>    m_regions;
    std::vector<Candidate> m_candidates;
    int                    m_filter_ticks = 0;
    std::chrono::steady_clock::time_point m_last_tick;

    ~Impl() { CloseProcess(); }

    void CloseProcess()
    {
        if (m_process) { CloseHandle(m_process); m_process = nullptr; }
        Reset();
    }

    void Reset(bool skip_cooldown = false)
    {
        m_position_addr     = 0;
        m_regions.clear();
        m_candidates.clear();
        m_scan_queue.clear();
        m_scan_done         = false;
        m_scan_started      = false;
        m_scan_progress     = 0;
        m_filter_ticks      = 0;
        m_is_playing   = false;
        m_paused_ticks = 0;
        if (skip_cooldown)
            m_last_scan_time = {}; // reset to epoch so BeginScan runs on the very next tick
        // else: preserve m_last_scan_time to keep cooldown active and avoid scan thrashing
    }

    // Opens the GoPlayAlong process with the largest working set (main process,
    // not one of the helper sub-processes).
    bool EnsureProcess()
    {
        if (m_process)
        {
            DWORD code = 0;
            if (GetExitCodeProcess(m_process, &code) && code == STILL_ACTIVE)
                return true;
            CloseProcess();
        }

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;

        PROCESSENTRY32W e{};
        e.dwSize = sizeof(e);
        DWORD  best_pid = 0;
        SIZE_T best_mem = 0;

        if (Process32FirstW(snap, &e))
        {
            do {
                if (wcscmp(e.szExeFile, PROCESS_NAME) != 0) continue;

                HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, e.th32ProcessID);
                if (!h) continue;

                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)) && pmc.WorkingSetSize > best_mem)
                {
                    best_mem = pmc.WorkingSetSize;
                    best_pid = e.th32ProcessID;
                }
                CloseHandle(h);
            } while (Process32NextW(snap, &e));
        }
        CloseHandle(snap);

        if (!best_pid) return false;
        m_process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, best_pid);
        return m_process != nullptr;
    }

    bool ReadDouble(uintptr_t addr, double& out) const
    {
        SIZE_T n = 0;
        return ::ReadProcessMemory(m_process, reinterpret_cast<LPCVOID>(addr), &out, 8, &n) && n == 8;
    }

    // Phase 1a — fast: walk the virtual address space with VirtualQueryEx and queue
    // every scannable region. No ReadProcessMemory calls, completes in microseconds.
    void BeginScan()
    {
        m_scan_queue.clear();
        m_scan_progress = 0;
        m_regions.clear();
        m_candidates.clear();

        constexpr DWORD EXEC_FLAGS = PAGE_EXECUTE | PAGE_EXECUTE_READ
                                   | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        constexpr DWORD READ_FLAGS = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY;

        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0;

        while (VirtualQueryEx(m_process, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t next = base + mbi.RegionSize;

            if ((mbi.State == MEM_COMMIT)
                && (mbi.Protect & READ_FLAGS)
                && !(mbi.Protect & EXEC_FLAGS)
                && !(mbi.Protect & PAGE_GUARD)
                && (mbi.RegionSize >= 8)
                && (mbi.RegionSize <= 64ULL * 1024 * 1024))
            {
                m_scan_queue.push_back(mbi);
            }

            if (next <= addr) break;
            addr = next;
        }

        m_last_scan_time = std::chrono::steady_clock::now();
    }

    // Phase 1b — reads up to SCAN_BYTES_PER_TICK of queued regions per call,
    // so each timer tick blocks REAPER for at most ~2 ms instead of seconds.
    // Returns true when all queued regions have been processed.
    bool ContinueScan()
    {
        SIZE_T bytes_this_tick = 0;

        while (m_scan_progress < m_scan_queue.size() && bytes_this_tick < SCAN_BYTES_PER_TICK)
        {
            const auto& mbi = m_scan_queue[m_scan_progress++];
            bytes_this_tick += mbi.RegionSize;

            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            std::vector<BYTE> buf(mbi.RegionSize);
            SIZE_T n = 0;
            if (!::ReadProcessMemory(m_process, mbi.BaseAddress, buf.data(), mbi.RegionSize, &n) || n < 8)
                continue;

            size_t ri     = m_regions.size();
            bool   pushed = false;

            for (size_t i = 0; i + 8 <= n; i += 8)
            {
                double val;
                memcpy(&val, buf.data() + i, 8);
                if (std::isfinite(val) && val >= MIN_POSITION && val <= MAX_DURATION)
                {
                    if (!pushed)
                    {
                        m_regions.push_back({ base, static_cast<SIZE_T>(n) });
                        pushed = true;
                    }
                    m_candidates.push_back({ base + i, val, 0, ri, i });
                }
            }
        }

        return m_scan_progress >= m_scan_queue.size();
    }

    // Re-reads each candidate region and updates ticks based on rate of change.
    // A candidate that advances at ~1.0 s/s (±RATE_TOLERANCE) increments its
    // tick counter. After CONFIRM_TICKS consistent ticks the address is committed.
    // Paused playback (delta ≈ 0) neither increments nor resets ticks.
    void FilterCandidates()
    {
        auto   now = std::chrono::steady_clock::now();
        double dt  = std::chrono::duration<double>(now - m_last_tick).count();
        m_last_tick = now;
        ++m_filter_ticks;

        if (dt < 0.005 || dt > 2.0) return;

        for (size_t ri = 0; ri < m_regions.size(); ++ri)
        {
            std::vector<BYTE> buf(m_regions[ri].size);
            SIZE_T n = 0;
            bool ok = ::ReadProcessMemory(m_process,
                reinterpret_cast<LPCVOID>(m_regions[ri].base),
                buf.data(), m_regions[ri].size, &n);

            for (auto& c : m_candidates)
            {
                if (c.region_idx != ri || c.ticks < 0) continue;

                if (!ok || c.region_offset + 8 > n) { c.ticks = -1; continue; }

                double new_val;
                memcpy(&new_val, buf.data() + c.region_offset, 8);

                if (!std::isfinite(new_val) || new_val < 0.0 || new_val > MAX_DURATION)
                {
                    c.ticks = -1;
                    continue;
                }

                double delta = new_val - c.last_value;
                c.last_value = new_val;

                if (delta >= dt * (1.0 - RATE_TOLERANCE) && delta <= dt * (1.0 + RATE_TOLERANCE))
                    ++c.ticks;
                else if (delta < -0.5 || delta > dt * 4.0)
                    c.ticks = 0; // bad jump: reset but keep candidate
                // delta ≈ 0 (paused): leave ticks unchanged
            }
        }

        m_candidates.erase(
            std::remove_if(m_candidates.begin(), m_candidates.end(),
                [](const Candidate& c) { return c.ticks < 0; }),
            m_candidates.end());

        auto best = std::max_element(m_candidates.begin(), m_candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.ticks < b.ticks; });

        if (best != m_candidates.end() && best->ticks >= CONFIRM_TICKS)
        {
            m_position_addr = best->address;
            m_candidates.clear();
            m_regions.clear();
            return;
        }

        // Trigger a fresh scan if candidates ran out or discovery is taking too long
        if (m_candidates.empty() || m_filter_ticks >= RESCAN_INTERVAL)
        {
            m_scan_done    = false;
            m_scan_started = false;
        }
    }

    GoPlayAlongState ReadProcessMemory()
    {
        GoPlayAlongState state{};
        state.play_rate = 1.0;

        if (!EnsureProcess())
            throw std::runtime_error(
                "GoPlayAlong 4 not found.\n"
                "Open GoPlayAlong 4 with a song loaded and press play.\n");

        // Phase 1a: enumerate scannable regions (fast, no ReadProcessMemory).
        // Guarded by a cooldown so that rapid Reset() calls (e.g. stale address after a
        // song restart) don't hammer the process with repeated scans.
        if (!m_scan_done && !m_scan_started)
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - m_last_scan_time).count();
            if (elapsed < MIN_SCAN_INTERVAL_SEC)
                return state;
            BeginScan();
            m_scan_started = true;
            return state;
        }

        // Phase 1b: read regions incrementally — at most SCAN_BYTES_PER_TICK per tick,
        // so REAPER is never blocked for more than ~2 ms regardless of process size.
        if (!m_scan_done)
        {
            if (ContinueScan())
            {
                m_scan_done    = true;
                m_filter_ticks = 0;
                m_last_tick    = std::chrono::steady_clock::now();
            }
            return state;
        }

        // Phase 2: rate-of-change filtering across timer ticks
        if (m_position_addr == 0)
        {
            FilterCandidates();
            return state; // GoPlayAlong must be playing for discovery to complete
        }

        // Phase 3: steady-state position read
        double position = 0.0;
        if (!ReadDouble(m_position_addr, position)
            || !std::isfinite(position) || position < 0.0 || position > MAX_DURATION)
        {
            Reset(); // address gone (song changed or app restarted); redo discovery
            return state;
        }

        double delta = position - m_last_position;
        if (delta > 0.005 && delta < 0.5)
        {
            m_is_playing   = true;
            m_paused_ticks = 0;
        }
        else if (delta < -1.0)
        {
            // Large backward jump: user rewound or song restarted.
            // Reset counter so the next advancing tick quickly re-detects play.
            m_paused_ticks = 0;
        }
        else if (++m_paused_ticks > PAUSED_TICKS_THRESHOLD)
        {
            m_is_playing = false;
        }
        m_last_position = position;

        // If position hasn't advanced for STALE_ADDRESS_THRESHOLD ticks (~3 s) AND is
        // near zero, the address is probably stale: GoPlayAlong auto-rewound to the
        // beginning and V8 GC moved the object. Force immediate rediscovery.
        // Mid-song pauses (position >= MIN_POSITION) are left alone — the address is
        // still valid and should resume instantly when GoPlayAlong plays again.
        if (m_paused_ticks > STALE_ADDRESS_THRESHOLD && position < MIN_POSITION)
        {
            Reset(true);
            return state;
        }

        state.play_position = position;
        state.play_state    = m_is_playing;
        return state;
    }
};

GoPlayAlong::GoPlayAlong()  : m_impl(std::make_unique<Impl>()) {}
GoPlayAlong::~GoPlayAlong() = default;

GoPlayAlongState GoPlayAlong::ReadProcessMemory()
{
    return m_impl->ReadProcessMemory();
}

} // namespace tnt
