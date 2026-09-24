#include "own_window.hpp"

#include <algorithm>

void OwnWindow::record_input(InputType type, int32_t a, int32_t b, int32_t c, int32_t d) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (type == InputType::Motion && !input_events_.empty() &&
        input_events_.back().type == InputType::Motion) {
        input_events_.back().a = a;
        input_events_.back().b = b;
        return;
    }
    if (input_events_.size() >= kMaxInputEvents) {
        // Once the queue is full, preserving a transition while discarding the
        // motion that located it can replay a click at the wrong coordinates.
        // Mark the batch unusable and let the client reset held state instead.
        input_events_.clear();
        input_overflowed_ = true;
    }
    input_events_.push_back({next_input_sequence_++, type, a, b, c, d});
}

std::vector<OwnWindow::InputEvent> OwnWindow::take_input_events(uint32_t max_events,
                                                                bool* overflowed) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (input_overflowed_) {
        // A discarded transition may have been a release. Never replay retained
        // presses after telling the client to reset all state.
        input_events_.clear();
        if (overflowed) *overflowed = true;
        input_overflowed_ = false;
        return {};
    }
    const size_t count = std::min<size_t>(max_events, input_events_.size());
    std::vector<InputEvent> events;
    events.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        events.push_back(input_events_.front());
        input_events_.pop_front();
    }
    if (overflowed) *overflowed = false;
    return events;
}
