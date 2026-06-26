#pragma once

#include <memory>
#include <string>

namespace tnt {

enum class ReaperPlayState
{
    STOPPED,
    PLAYING,
    PAUSED,
};

enum class ReaperToggleCommand
{
    PRESERVE_PITCH,
};

// C++ wrapper around C-style REAPER API functions
class Reaper final
{
public:
    Reaper();
    ~Reaper();

    double GetOutputLatency() const;
    double GetPlayPosition() const;
    double GetPlayRate() const;
    ReaperPlayState GetPlayState() const;
    bool GetToggleCommandState(const ReaperToggleCommand& command) const;

    void SetEditCursorPosition(const double time, const bool move_view, const bool seek_play) const;
    void SetPlayRate(const double play_rate) const;
    void SetPlayState(const ReaperPlayState& play_state) const;
    void SetRepeat(const bool repeat) const;
    void SetTimeSelection(const double start_time, const double end_time) const;
    void ShowConsoleMessage(const std::string& message) const;
    void ToggleCommand(const ReaperToggleCommand& command) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
