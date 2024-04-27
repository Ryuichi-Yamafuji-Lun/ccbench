#pragma once

#include <atomic>
#include <chrono>

// Global Timer for long transaction checking
class GlobalTimer{
private:
std::atomic<bool> active;
std::atomic<std::chrono::steady_clock::time_point> start_time;
public:
// Set global_timer
GlobalTimer(): active(false), start_time(std::chrono::steady_clock::now()) {}

// Begin global_timer
void start() {
    active = true;
    start_time.store(std::chrono::steady_clock::now());
}

// Stop global_timer
void stop() {
    active = false;
}

// Reset global_timer to 0
void reset() {
    start_time.store(std::chrono::steady_clock::now());
}

// Elapsed Time in milliseconds
std::chrono::milliseconds elapsed() const {
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - start_time.load());
    return duration;
}

};