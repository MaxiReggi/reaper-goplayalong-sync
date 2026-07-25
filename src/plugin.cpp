#include "plugin.h"

#include "goplayalong.h"
#include "reaper.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace tnt {

// REAPER runs the MainLoop ~30 times/second.
static constexpr double MINIMUM_TIME_STEP = 0.001;                 // seconds
static constexpr double MINIMUM_PLAY_RATE_STEP = 0.001;
static constexpr double GOPLAYALONG_CURSOR_JUMP_THRESHOLD = 0.1;   // seconds
static constexpr double LOOP_BOUNDARY_TOLERANCE = 0.06;            // seconds; don't touch anything this close to a loop edge
// GPA updates position at ~15 Hz; 15 consecutive non-advancing ticks ≈ 500 ms of no movement.
static constexpr int NOT_ADVANCING_STOP_THRESHOLD = 15;
// Cap dead reckoning extrapolation to avoid runaway drift between GPA updates.
static constexpr double DEAD_RECKONING_MAX_EXTRAPOLATION = 0.2;    // seconds

// Position servo: instead of waiting for drift to accumulate and then jumping REAPER's
// cursor (which leaves a window of a couple seconds where the tab is visibly out of sync
// with the audio before the jump happens), continuously nudge REAPER's play rate a tiny,
// inaudible amount toward GoPlayAlong's position every tick. This keeps the drift from
// ever growing large enough to be noticeable, instead of correcting it after the fact.
static constexpr double POSITION_SERVO_MAX_RATE_NUDGE = 0.004;     // max ±0.4% deviation from nominal tempo
static constexpr double POSITION_SERVO_JUMP_THRESHOLD  = 0.25;     // seconds; beyond this, drift is treated as a
                                                                    // real discontinuity (stall/glitch) — jump instead
// Error at which the nudge saturates at POSITION_SERVO_MAX_RATE_NUDGE. A previous attempt
// at 0.03s converged fast but caused audible artifacts — REAPER's preserve-pitch time
// stretch doesn't like the rate being re-issued too often/aggressively. 0.09s is a middle
// ground: noticeably stronger than treating JUMP_THRESHOLD itself as the saturation point,
// without hammering the rate as hard as 0.03s did.
static constexpr double POSITION_SERVO_SATURATION_ERROR = 0.09;   // seconds
static constexpr double POSITION_SERVO_GAIN = POSITION_SERVO_MAX_RATE_NUDGE / POSITION_SERVO_SATURATION_ERROR;
// Only re-issue SetPlayRate when the target moved by at least this much, so the time
// stretch engine isn't asked to recompute on every single tick for negligible changes.
static constexpr double POSITION_SERVO_UPDATE_STEP = 0.0015;

struct Plugin::Impl final
{
    Impl(PluginState& plugin_state)
        : m_plugin_state(plugin_state)
    {}

    void MainLoop()
    {
        try
        {
            m_goplayalong_state = m_goplayalong.ReadProcessMemory();
        }
        catch (const std::runtime_error& error)
        {
            if (m_last_error != error.what())
            {
                m_reaper.ShowConsoleMessage(error.what());
                m_last_error = error.what();
            }
            return;
        }

        if (!m_last_error.empty())
        {
            m_reaper.ShowConsoleMessage("Successfully connected to GoPlayAlong process.\n");
            m_last_error = "";
        }

        UpdateDeadReckoning();

        if (m_goplayalong_state.play_state)
        {
            SyncLoopState();
            SyncTimeSelection();
            SyncPlayPosition();
            SyncPlayRate();
        }
        else if (ReaperStoppedOrPaused())
        {
            if (GoPlayAlongLoopStateChanged())
            {
                SyncLoopState();
            }

            if (GoPlayAlongTimeSelectionChanged() && m_goplayalong_state.time_selection_end_position > MINIMUM_PLAY_RATE_STEP)
            {
                SyncTimeSelection();
                SetPlayPosition(m_goplayalong_state.time_selection_start_position);
            }
            else if (GoPlayAlongCursorMoved())
            {
                SyncTimeSelection();
                SetPlayPosition(m_goplayalong_state.play_position);
            }

            SyncPlayRate();
        }

        SyncPlayState();

        m_prev_goplayalong_state = m_goplayalong_state;
    }

private:
    // Dead reckoning: track the last GPA position update and extrapolate forward
    // using elapsed real time × play rate. This gives a smooth real-time estimate
    // of GPA's current position between its 15 Hz memory updates, so the position
    // servo has a stable error signal to correct against instead of stale/jumpy data.
    void UpdateDeadReckoning()
    {
        if (!CompareDoubles(m_goplayalong_state.play_position, m_gpa_reckoned_position, MINIMUM_TIME_STEP))
        {
            m_gpa_reckoned_position = m_goplayalong_state.play_position;
            m_gpa_reckoned_time = std::chrono::steady_clock::now();
            m_gpa_reckoning_valid = true;
        }
    }

    double GetDeadReckonedPosition() const
    {
        if (!m_gpa_reckoning_valid || !m_goplayalong_state.play_state)
            return m_goplayalong_state.play_position;

        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_gpa_reckoned_time).count();
        const double capped = elapsed < DEAD_RECKONING_MAX_EXTRAPOLATION ? elapsed : DEAD_RECKONING_MAX_EXTRAPOLATION;
        return m_gpa_reckoned_position + capped * m_goplayalong_state.play_rate;
    }

    void SyncLoopState()
    {
        if (m_goplayalong_state.loop_state && !(m_goplayalong_state.play_state && m_goplayalong_state.count_in_state))
        {
            m_reaper.SetRepeat(true);
        }
        else
        {
            m_reaper.SetRepeat(false);
        }
    }

    void SyncTimeSelection()
    {
        m_reaper.SetTimeSelection(
            m_goplayalong_state.time_selection_start_position,
            m_goplayalong_state.time_selection_end_position);
    }

    void SyncPlayPosition()
    {
        const double gpa_pos = GetDeadReckonedPosition();
        const double reaper_pos = m_reaper.GetPlayPosition();

        // Do not touch anything right at a loop boundary — let GoPlayAlong's own loop
        // reset settle first, and drop any lingering rate nudge so it doesn't carry over.
        if (CompareDoubles(reaper_pos, m_goplayalong_state.time_selection_start_position, LOOP_BOUNDARY_TOLERANCE)
         || CompareDoubles(reaper_pos, m_goplayalong_state.time_selection_end_position, LOOP_BOUNDARY_TOLERANCE))
        {
            ResetPlayRateToNominal();
            return;
        }

        // Follow intentional seeks in GoPlayAlong immediately — a servo nudge would take
        // too long to catch up to a deliberate jump, so just cut over.
        if (!CompareDoubles(m_prev_goplayalong_state.play_position, m_goplayalong_state.play_position, GOPLAYALONG_CURSOR_JUMP_THRESHOLD))
        {
            SetPlayPosition(gpa_pos + m_reaper.GetOutputLatency());
            return;
        }

        const double error = (gpa_pos + m_reaper.GetOutputLatency()) - reaper_pos;

        if (fabs(error) > POSITION_SERVO_JUMP_THRESHOLD)
        {
            // Drift got too large for a smooth correction (e.g. a stall or a glitched
            // read) — fall back to a hard jump rather than nudging for a long time.
            SetPlayPosition(gpa_pos + m_reaper.GetOutputLatency());
            return;
        }

        EnablePreservePitch();

        const double correction = std::clamp(error * POSITION_SERVO_GAIN, -POSITION_SERVO_MAX_RATE_NUDGE, POSITION_SERVO_MAX_RATE_NUDGE);
        const double target_rate = m_goplayalong_state.play_rate * (1.0 + correction);

        if (!CompareDoubles(m_reaper.GetPlayRate(), target_rate, POSITION_SERVO_UPDATE_STEP))
        {
            m_reaper.SetPlayRate(target_rate);
        }
    }

    // Cancels any active servo rate nudge, returning REAPER to GoPlayAlong's exact nominal tempo.
    void ResetPlayRateToNominal()
    {
        if (m_goplayalong_state.play_rate > MINIMUM_PLAY_RATE_STEP
         && !CompareDoubles(m_reaper.GetPlayRate(), m_goplayalong_state.play_rate, MINIMUM_PLAY_RATE_STEP))
        {
            m_reaper.SetPlayRate(m_goplayalong_state.play_rate);
        }
    }

    // Handles deliberate tempo changes in GoPlayAlong (50/60/70/80/90/100%). Gated on
    // GoPlayAlongPlayRateChanged() rather than comparing against REAPER's live rate,
    // since the position servo intentionally keeps REAPER's live rate slightly off the
    // nominal tempo — comparing directly would fight the servo and pause playback every tick.
    void SyncPlayRate()
    {
        if (m_goplayalong_state.play_rate > MINIMUM_PLAY_RATE_STEP && GoPlayAlongPlayRateChanged())
        {
            EnablePreservePitch();
            m_reaper.SetPlayState(ReaperPlayState::PAUSED);
            m_reaper.SetPlayRate(m_goplayalong_state.play_rate);
        }
    }

    void SyncPlayState()
    {
        const bool ps             = m_goplayalong_state.play_state;
        const bool pps            = m_prev_goplayalong_state.play_state;
        const bool advancing      = m_goplayalong_state.play_position > m_prev_goplayalong_state.play_position + MINIMUM_TIME_STEP;
        const bool reaper_stopped = ReaperStoppedOrPaused();

        if (!reaper_stopped)
            m_not_advancing_ticks = advancing ? 0 : m_not_advancing_ticks + 1;
        else
            m_not_advancing_ticks = 0;

        // Position-based stop: GPA position frozen for N ticks means GPA is paused/stopped.
        // This handles songs where play_state stays 1 even when GPA is not playing.
        if (!reaper_stopped && m_not_advancing_ticks >= NOT_ADVANCING_STOP_THRESHOLD)
        {
            m_not_advancing_ticks = 0;
            m_reaper.SetPlayState(ReaperPlayState::STOPPED);
            return;
        }

        if (ps)
        {
            if (m_goplayalong_state.count_in_state
             && (!GoPlayAlongCursorMoved() || (m_goplayalong_state.time_selection_start_position > MINIMUM_TIME_STEP
                 && m_prev_goplayalong_state.play_position < MINIMUM_TIME_STEP)))
            {
                if (!CompareDoubles(m_reaper.GetPlayPosition(), m_goplayalong_state.time_selection_start_position, MINIMUM_TIME_STEP)
                 && m_reaper.GetPlayPosition() < m_goplayalong_state.time_selection_end_position)
                {
                    return;
                }
                m_reaper.SetPlayState(ReaperPlayState::STOPPED);
            }
            else if (reaper_stopped && advancing)
            {
                if (m_goplayalong_state.time_selection_start_position > MINIMUM_TIME_STEP)
                {
                    SetPlayPosition(m_goplayalong_state.time_selection_start_position + m_reaper.GetOutputLatency());
                }
                else
                {
                    SetPlayPosition(m_goplayalong_state.play_position + m_reaper.GetOutputLatency());
                }
                m_reaper.SetPlayState(ReaperPlayState::PLAYING);
            }
        }
        else if (!reaper_stopped && pps)
        {
            // play_state explicitly 0: stop immediately (normal GPA behavior)
            m_reaper.SetPlayState(ReaperPlayState::STOPPED);
        }
    }

    void SetPlayPosition(const double time)
    {
        m_reaper.SetEditCursorPosition(time, false, true);
    }

    bool CompareDoubles(const double val1, const double val2, const double epsilon) const
    {
        return fabs(val1 - val2) < epsilon;
    }

    bool GoPlayAlongLoopStateChanged() const
    {
        return m_goplayalong_state.loop_state != m_prev_goplayalong_state.loop_state;
    }

    bool GoPlayAlongTimeSelectionChanged() const
    {
        return !CompareDoubles(m_goplayalong_state.time_selection_start_position, m_prev_goplayalong_state.time_selection_start_position, MINIMUM_TIME_STEP)
            || !CompareDoubles(m_goplayalong_state.time_selection_end_position, m_prev_goplayalong_state.time_selection_end_position, MINIMUM_TIME_STEP);
    }

    bool GoPlayAlongCursorMoved() const
    {
        return !CompareDoubles(m_goplayalong_state.play_position, m_prev_goplayalong_state.play_position, MINIMUM_TIME_STEP);
    }

    bool GoPlayAlongPlayRateChanged() const
    {
        return !CompareDoubles(m_goplayalong_state.play_rate, m_prev_goplayalong_state.play_rate, MINIMUM_PLAY_RATE_STEP);
    }

    bool ReaperStoppedOrPaused() const
    {
        switch (m_reaper.GetPlayState())
        {
        case ReaperPlayState::STOPPED:
        case ReaperPlayState::PAUSED:
            return true;
        case ReaperPlayState::PLAYING:
            return false;
        default:
            throw std::runtime_error("REAPER is in an invalid play state.\n");
        }
    }

    void EnablePreservePitch() const
    {
        if (!m_reaper.GetToggleCommandState(ReaperToggleCommand::PRESERVE_PITCH))
        {
            m_reaper.ToggleCommand(ReaperToggleCommand::PRESERVE_PITCH);
        }
    }

    PluginState& m_plugin_state;
    GoPlayAlong m_goplayalong;
    Reaper m_reaper;

    GoPlayAlongState m_prev_goplayalong_state;
    GoPlayAlongState m_goplayalong_state;

    int m_not_advancing_ticks = 0;

    std::string m_last_error;

    double m_gpa_reckoned_position = 0.0;
    std::chrono::steady_clock::time_point m_gpa_reckoned_time;
    bool m_gpa_reckoning_valid = false;
};

Plugin::Plugin(PluginState& plugin_state)
    : m_impl(std::make_unique<Impl>(plugin_state))
{}

Plugin::~Plugin() = default;

void Plugin::MainLoop()
{
    m_impl->MainLoop();
}

}
