#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Regression uses valid readings at 100 ms intervals. Flow is mL/hour
// assuming a liquid density of 1 g/mL.
class WeightFlowStats {
public:
    static constexpr int32_t MIN_WGT = 50;
    static constexpr int64_t STDEV_MIN = 8;
    static constexpr size_t AVG_SIZE = 25;
    static constexpr size_t STDEV_SIZE = 100;

    enum class State : uint8_t {
        waiting, running, stable, target_reached, timed_out
    };

    struct Result {
        State state;
        int64_t avg_flow;
        int64_t flow_dev_avg;
        uint32_t cnt;
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
        const int64_t ml_hour = (360 * numerator) / denominator;
        push(flow_, flow_pos_, flow_count_, flow_sum_, ml_hour);
        avg_flow_ = flow_sum_ / static_cast<int64_t>(flow_count_);

        if (flow_count_ != AVG_SIZE) {
            return;
        }
        const int64_t difference = ml_hour - avg_flow_;
        const int64_t flow_dev = 10 * (difference < 0 ? -difference : difference);
        push(deviation_, deviation_pos_, deviation_count_, deviation_sum_, flow_dev);
        flow_dev_avg_ = deviation_sum_ / static_cast<int64_t>(deviation_count_);
        if (deviation_count_ == STDEV_SIZE && flow_dev_avg_ < STDEV_MIN) {
            state_ = State::stable;
        }
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
        return {state_, avg_flow_, flow_dev_avg_, cnt_};
    }

private:
    template <size_t N>
    static void push(std::array<int64_t, N>& buffer,
                     size_t& position, size_t& count,
                     int64_t& sum, int64_t value) {
        if (count == N) {
            sum -= buffer[position];
        } else {
            ++count;
        }
        buffer[position] = value;
        sum += value;
        position = (position + 1) % N;
    }

    State state_ = State::waiting;
    uint32_t cnt_ = 0;
    int64_t sum_x_ = 0;
    int64_t sum_y_ = 0;
    int64_t sum_xy_ = 0;
    int64_t sum_xx_ = 0;
    std::array<int64_t, AVG_SIZE> flow_{};
    size_t flow_pos_ = 0;
    size_t flow_count_ = 0;
    int64_t flow_sum_ = 0;
    int64_t avg_flow_ = 0;
    std::array<int64_t, STDEV_SIZE> deviation_{};
    size_t deviation_pos_ = 0;
    size_t deviation_count_ = 0;
    int64_t deviation_sum_ = 0;
    int64_t flow_dev_avg_ = 0;
};
