#include "idle.hpp"

namespace hc::network::detail {

uint32_t idle_ticks(std::atomic<uint32_t> &at, uint32_t now, uint32_t delay)
{
    uint32_t stamp = at.load();
    const auto since = static_cast<int32_t>(now - stamp);
    if (since <= static_cast<int32_t>(delay)) {
        // a stamp of another task can be a tick newer than `now`
        return since > 0 ? static_cast<uint32_t>(since) : 0;
    }
    if (at.compare_exchange_strong(stamp, now - delay)) {
        return delay;
    }
    // another task stamped an activity since the load: `stamp` holds it now
    const auto fresh = static_cast<int32_t>(now - stamp);
    return fresh > 0 ? static_cast<uint32_t>(fresh) : 0;
}

} // namespace hc::network::detail
