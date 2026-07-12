#pragma once
#include <chrono>
#include <cstdio>
#include <string>

struct Timer {
    using Clock = std::chrono::steady_clock;

    explicit Timer(std::string label = "")
        : label_(std::move(label))
        , start_(Clock::now())
    {
    }

    double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    }

    void report() const {
        printf("[%-20s] %8.1f ms\n", label_.c_str(), elapsed_ms());
    }

    void reset() {
        start_ = Clock::now();
    }

private:
    std::string label_;
    Clock::time_point start_;
};
