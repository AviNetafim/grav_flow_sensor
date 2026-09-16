#pragma once

#include <cstdint>

// Regression uses valid readings at 100 ms intervals. Flow is mL/hour
// assuming a liquid density of 1 g/mL.
class WeightFlowStats {
public:
    static constexpr int32_t MIN_WGT = 50;
    enum class State : uint8_t {
        waiting, running, target_reached, timed_out
    };

    struct Result {
        State state;
        uint32_t cnt;
        int64_t flow;
    };

    bool should_start(int32_t weight_10mg) const {
        return state_ == State::waiting && weight_10mg > MIN_WGT;
    }

    void start(int32_t weight_10mg) {
        *this = WeightFlowStats{};
        state_ = State::running;
        calculate(weight_10mg);
    }

    void calculate(int32_t weight_10mg) {
        if (state_ != State::running) {
            return;
        }
        const int64_t x = ++cnt_;
        const int64_t y = weight_10mg;
        sum_x_ += x;
        sum_y_ += y;
        sum_xy_ += x * y;
        sum_xx_ += x * x;

        if (cnt_ < 2) {
            return; // One point cannot define a regression slope.
        }
        const int64_t numerator = x * sum_xy_ - sum_x_ * sum_y_;
        const int64_t denominator = x * sum_xx_ - sum_x_ * sum_x_;
        slope_ = numerator / denominator;
        flow_ = (360 * numerator) / denominator;
    }

    int64_t slope() const {
        return slope_;
    }

    void stop(bool target_reached, bool timed_out) {
        if (state_ != State::waiting && state_ != State::running) {
            return;
        }
        if (target_reached) {
            state_ = State::target_reached;
        } else if (timed_out) {
            state_ = State::timed_out;
        }
    }

    bool finished() const {
        return state_ != State::waiting && state_ != State::running;
    }

    Result result() const {
        return {state_, cnt_, flow_};
    }

private:
    State state_ = State::waiting;
    uint32_t cnt_ = 0;
    int64_t sum_x_ = 0;
    int64_t sum_y_ = 0;
    int64_t sum_xy_ = 0;
    int64_t sum_xx_ = 0;
    int64_t slope_ = 0;
    int64_t flow_ = 0;
};
