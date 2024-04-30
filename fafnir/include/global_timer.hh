#pragma once

#include <atomic>
#include <chrono>

// Global Timer for long transaction checking
class GlobalTimer{
private:
    std::chrono::steady_clock::time_point start_time;
public:
    // Set global_timer
    GlobalTimer(): start_time(std::chrono::steady_clock::now()) {}

    // Begin global_timer
    void start() {
        start_time = std::chrono::steady_clock::now();
    }

    // Reset global_timer to 0
    void reset() {
        start_time = std::chrono::steady_clock::now();
    }

    // Elapsed Time in milliseconds
    std::chrono::milliseconds elapsed() const {
        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - start_time);
        return duration;
    }

};