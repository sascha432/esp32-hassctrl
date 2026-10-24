#pragma once

// The bytes of a file on their way from a download to the player (docs/architecture.md section
// 9.6): one task writes what arrives, another reads it while it plays. Neither waits on the other:
// the reader takes what is there, the writer puts what fits. Plain C++ with atomics, host tested.
//
// A seek of the reader goes through the writer, which has to fetch the file again from there:
// seek() asks for it, take_seek() hands the offset to the writer, restart() empties the ring and
// tells the reader that the bytes come from the new offset now. The reader reads nothing while a
// seek is on its way. cancel() tells the writer that the reader is gone.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace hc::core {

class ByteRing {
public:
    // `capacity` bytes from the heap (PSRAM on the device: a block that large goes there)
    explicit ByteRing(size_t capacity);

    // the memory came
    bool valid() const
    {
        return data_ != nullptr;
    }
    size_t capacity() const
    {
        return capacity_;
    }

    // ------------------------------------------------------------------------- the writer

    // What fits of `data`, from the current position on.
    size_t write(const uint8_t *data, size_t size);
    size_t space() const;
    // The file's whole length, once the download knows it.
    void set_size(uint32_t size);
    // No more bytes come: the file is complete, or the download failed for good.
    void finish(bool failed);
    // A seek of the reader to fetch from: true once per seek().
    bool take_seek(uint32_t &offset);
    // Empties the ring after a seek: the next bytes written are those from `offset`.
    void restart(uint32_t offset);
    bool cancelled() const
    {
        return cancelled_.load(std::memory_order_acquire);
    }

    // ------------------------------------------------------------------------- the reader

    // What is there, up to `max`; 0 while a seek is on its way.
    size_t read(uint8_t *out, size_t max);
    size_t available() const;
    // The file's offset of the next byte read().
    uint32_t position() const
    {
        return position_.load(std::memory_order_acquire);
    }
    // 0 while it is not known
    uint32_t size() const
    {
        return size_.load(std::memory_order_acquire);
    }
    // No more bytes come than those in the ring (finish() was called).
    bool finished() const
    {
        return !seeking() && finished_.load(std::memory_order_acquire);
    }
    // Everything there is was read: the end of the file, or of a failed download.
    bool ended() const;
    bool failed() const
    {
        return failed_.load(std::memory_order_acquire);
    }
    // Drops what is in the ring and asks the writer for the file from `offset`.
    void seek(uint32_t offset);
    bool seeking() const
    {
        return seek_wanted_.load(std::memory_order_acquire) || seek_taken_.load(std::memory_order_acquire);
    }
    void cancel()
    {
        cancelled_.store(true, std::memory_order_release);
    }

private:
    std::unique_ptr<uint8_t[]> data_;
    size_t capacity_ = 0;
    // Bytes ever written and read since the last restart(): the difference is what the ring
    // holds, each modulo the capacity its place. Written only by its own side.
    std::atomic<size_t> written_{0};
    std::atomic<size_t> read_{0};
    std::atomic<uint32_t> position_{0};
    std::atomic<uint32_t> size_{0};
    std::atomic<bool> finished_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> cancelled_{false};
    // seek(): wanted until the writer takes it, taken until it restarted
    std::atomic<uint32_t> seek_offset_{0};
    std::atomic<bool> seek_wanted_{false};
    std::atomic<bool> seek_taken_{false};
};

} // namespace hc::core
