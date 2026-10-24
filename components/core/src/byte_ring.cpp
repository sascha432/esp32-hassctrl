#include "hc/core/byte_ring.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace hc::core {

ByteRing::ByteRing(size_t capacity) : data_(new (std::nothrow) uint8_t[capacity]), capacity_(data_ ? capacity : 0)
{
}

size_t ByteRing::space() const
{
    return capacity_ - (written_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire));
}

size_t ByteRing::write(const uint8_t *data, size_t size)
{
    const size_t written = written_.load(std::memory_order_relaxed);
    const size_t count = std::min(size, space());
    const size_t at = written % capacity_;
    const size_t first = std::min(count, capacity_ - at);
    std::memcpy(data_.get() + at, data, first);
    std::memcpy(data_.get(), data + first, count - first);
    written_.store(written + count, std::memory_order_release);
    return count;
}

void ByteRing::set_size(uint32_t size)
{
    size_.store(size, std::memory_order_release);
}

void ByteRing::finish(bool failed)
{
    failed_.store(failed, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
}

bool ByteRing::take_seek(uint32_t &offset)
{
    if (!seek_wanted_.load(std::memory_order_acquire)) {
        return false;
    }
    // taken before wanted is cleared: seeking() stays true in between
    seek_taken_.store(true, std::memory_order_release);
    offset = seek_offset_.load(std::memory_order_acquire);
    seek_wanted_.store(false, std::memory_order_release);
    return true;
}

void ByteRing::restart(uint32_t offset)
{
    // the reader reads nothing while seek_taken_ is set
    written_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
    position_.store(offset, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    // a newer seek() came meanwhile: it stays wanted
    seek_taken_.store(false, std::memory_order_release);
}

size_t ByteRing::available() const
{
    if (seeking()) {
        return 0;
    }
    return written_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
}

size_t ByteRing::read(uint8_t *out, size_t max)
{
    const size_t count = std::min(max, available());
    const size_t done = read_.load(std::memory_order_relaxed);
    const size_t at = done % capacity_;
    const size_t first = std::min(count, capacity_ - at);
    std::memcpy(out, data_.get() + at, first);
    std::memcpy(out + first, data_.get(), count - first);
    read_.store(done + count, std::memory_order_release);
    position_.store(position_.load(std::memory_order_relaxed) + static_cast<uint32_t>(count),
                    std::memory_order_release);
    return count;
}

bool ByteRing::ended() const
{
    // finished before the last bytes are counted: what the writer wrote before finish() is seen
    return !seeking() && finished_.load(std::memory_order_acquire) && available() == 0;
}

void ByteRing::seek(uint32_t offset)
{
    seek_offset_.store(offset, std::memory_order_release);
    seek_wanted_.store(true, std::memory_order_release);
}

} // namespace hc::core
