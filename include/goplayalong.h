#pragma once

#include <memory>

namespace tnt {

struct GoPlayAlongState
{
    double play_position = 0.0;             // seconds
    double time_selection_start_position = 0.0; // seconds
    double time_selection_end_position = 0.0;   // seconds
    double play_rate = 1.0;

    bool play_state = false;
    bool count_in_state = false;
    bool loop_state = false;
};

class GoPlayAlong final
{
public:
    GoPlayAlong();
    ~GoPlayAlong();

    // Reads playback state from GoPlayAlong process memory.
    // Throws std::runtime_error on failure.
    GoPlayAlongState ReadProcessMemory();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
