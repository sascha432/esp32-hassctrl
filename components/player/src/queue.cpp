#include "hc/player/queue.hpp"

#include <utility>

namespace hc::player {

namespace {

// xorshift32: a shuffle needs no more
uint32_t random_next(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// shuffles order[from..] in place (Fisher-Yates)
void shuffle_from(memory::Vector<uint32_t> &order, size_t from, uint32_t seed)
{
    uint32_t state = seed != 0 ? seed : 0x9e3779b9;
    for (size_t i = order.size(); i > from + 1; i--) {
        const size_t j = from + random_next(state) % (i - from);
        std::swap(order[i - 1], order[j]);
    }
}

} // namespace

void Queue::set(memory::Vector<Item> items, size_t start, bool shuffle, uint32_t seed)
{
    items_ = std::move(items);
    order_.clear();
    at_ = 0;
    if (items_.empty()) {
        return;
    }
    start = start < items_.size() ? start : 0;
    order_.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); i++) {
        order_.push_back(static_cast<uint32_t>(i));
    }
    if (shuffle) {
        // the chosen one first, the others shuffled after it
        std::swap(order_[0], order_[start]);
        shuffle_from(order_, 1, seed);
    } else {
        at_ = start;
    }
}

void Queue::clear()
{
    items_.clear();
    order_.clear();
    at_ = 0;
}

const Item *Queue::current() const
{
    return order_.empty() ? nullptr : &items_[order_[at_]];
}

bool Queue::advance(Repeat repeat)
{
    if (order_.empty()) {
        return false;
    }
    if (repeat == Repeat::One) {
        return true;
    }
    return next(repeat);
}

bool Queue::next(Repeat repeat)
{
    if (order_.empty()) {
        return false;
    }
    if (at_ + 1 < order_.size()) {
        at_++;
        return true;
    }
    if (repeat == Repeat::Off) {
        return false;
    }
    at_ = 0;
    return true;
}

bool Queue::previous(Repeat repeat)
{
    if (order_.empty()) {
        return false;
    }
    if (at_ > 0) {
        at_--;
        return true;
    }
    if (repeat == Repeat::All) {
        at_ = order_.size() - 1;
        return true;
    }
    return false;
}

void Queue::set_shuffle(bool shuffle, uint32_t seed)
{
    if (order_.empty()) {
        return;
    }
    const uint32_t playing = order_[at_];
    if (shuffle) {
        // the current one first, the others shuffled after it
        for (size_t i = 0; i < order_.size(); i++) {
            order_[i] = static_cast<uint32_t>(i);
        }
        std::swap(order_[0], order_[playing]);
        shuffle_from(order_, 1, seed);
        at_ = 0;
    } else {
        for (size_t i = 0; i < order_.size(); i++) {
            order_[i] = static_cast<uint32_t>(i);
        }
        at_ = playing;
    }
}

} // namespace hc::player
