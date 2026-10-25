#include "spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "hc/audio/audio.hpp"

namespace hc::audio {

namespace {

// 43 Hz per bin at 44.1 kHz
constexpr size_t kFftSize = 1024;
constexpr size_t kFftBits = 10;
// twice the FFT: the task writes one half while the screen reads the other
constexpr size_t kRingSize = 2 * kFftSize;
constexpr float kLowestHz = 40.0f;
constexpr float kFloorDb = -60.0f;
constexpr float kPi = 3.14159265f;

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
// PSRAM where there is some, allocated with the first file of the player and kept
int16_t *g_ring = nullptr;
size_t g_ring_end = 0;
uint32_t g_rate = 0; // 0: nothing plays

// the FFT's work arrays and its tables, only on the screen's task
struct Fft {
    float re[kFftSize];
    float im[kFftSize];
    float window[kFftSize];
    float cos[kFftSize / 2];
    float sin[kFftSize / 2];
};
Fft *g_fft = nullptr;

void *allocate(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

bool make_fft()
{
    if (g_fft != nullptr) {
        return true;
    }
    g_fft = static_cast<Fft *>(allocate(sizeof(Fft)));
    if (g_fft == nullptr) {
        return false;
    }
    for (size_t i = 0; i < kFftSize; i++) {
        // Hann
        g_fft->window[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / kFftSize);
    }
    for (size_t i = 0; i < kFftSize / 2; i++) {
        g_fft->cos[i] = std::cos(2.0f * kPi * static_cast<float>(i) / kFftSize);
        g_fft->sin[i] = -std::sin(2.0f * kPi * static_cast<float>(i) / kFftSize);
    }
    return true;
}

// in place, radix 2
void transform(float *re, float *im)
{
    for (size_t i = 0; i < kFftSize; i++) {
        size_t j = 0;
        for (size_t b = 0; b < kFftBits; b++) {
            j |= ((i >> b) & 1) << (kFftBits - 1 - b);
        }
        if (j > i) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t size = 2; size <= kFftSize; size *= 2) {
        const size_t half = size / 2;
        const size_t step = kFftSize / size;
        for (size_t start = 0; start < kFftSize; start += size) {
            for (size_t k = 0; k < half; k++) {
                const float wr = g_fft->cos[k * step];
                const float wi = g_fft->sin[k * step];
                const size_t a = start + k;
                const size_t b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

} // namespace

namespace detail {

void spectrum_feed(const int16_t *samples, size_t count, uint32_t rate)
{
    if (g_ring == nullptr) {
        return;
    }
    taskENTER_CRITICAL(&g_lock);
    for (size_t i = 0; i < count; i++) {
        g_ring[g_ring_end] = samples[i];
        g_ring_end = (g_ring_end + 1) % kRingSize;
    }
    g_rate = rate;
    taskEXIT_CRITICAL(&g_lock);
}

void spectrum_clear()
{
    taskENTER_CRITICAL(&g_lock);
    g_rate = 0;
    taskEXIT_CRITICAL(&g_lock);
}

// the ring before the first file of the player (play_media, on the caller's task)
bool spectrum_prepare()
{
    if (g_ring == nullptr) {
        auto *ring = static_cast<int16_t *>(allocate(kRingSize * sizeof(int16_t)));
        if (ring == nullptr) {
            return false;
        }
        std::memset(ring, 0, kRingSize * sizeof(int16_t));
        taskENTER_CRITICAL(&g_lock);
        g_ring = ring;
        taskEXIT_CRITICAL(&g_lock);
    }
    return true;
}

} // namespace detail

bool media_spectrum(float *bands, size_t count)
{
    if (count == 0 || g_ring == nullptr || !make_fft()) {
        return false;
    }
    static int16_t latest[kFftSize];
    taskENTER_CRITICAL(&g_lock);
    const uint32_t rate = g_rate;
    const size_t start = (g_ring_end + kRingSize - kFftSize) % kRingSize;
    const size_t first = std::min(kFftSize, kRingSize - start);
    std::memcpy(latest, g_ring + start, first * sizeof(int16_t));
    std::memcpy(latest + first, g_ring, (kFftSize - first) * sizeof(int16_t));
    taskEXIT_CRITICAL(&g_lock);
    if (rate == 0) {
        return false;
    }
    for (size_t i = 0; i < kFftSize; i++) {
        g_fft->re[i] = latest[i] / 32768.0f * g_fft->window[i];
        g_fft->im[i] = 0.0f;
    }
    transform(g_fft->re, g_fft->im);
    // a full scale sine through the Hann window: a peak of a quarter of the size
    const float full = kFftSize / 4.0f;
    const float bin_hz = static_cast<float>(rate) / kFftSize;
    const float top = static_cast<float>(rate) / 2.0f;
    const float ratio = std::pow(top / kLowestHz, 1.0f / static_cast<float>(count));
    float high = kLowestHz;
    // every band at least one bin of its own: the low bands are narrower than a bin and would
    // all show the same one, so they step a bin at a time until the octaves are wider
    size_t first_bin = 1;
    for (size_t band = 0; band < count; band++) {
        high *= ratio;
        const size_t end = std::max(first_bin + 1, static_cast<size_t>(high / bin_hz + 0.5f));
        const size_t last_bin = std::min(kFftSize / 2, end) - 1;
        float peak = 0.0f;
        for (size_t bin = first_bin; bin <= last_bin; bin++) {
            peak = std::max(peak, g_fft->re[bin] * g_fft->re[bin] + g_fft->im[bin] * g_fft->im[bin]);
        }
        const float db = 10.0f * std::log10(peak / (full * full) + 1e-12f);
        bands[band] = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
        first_bin = std::min(last_bin + 1, kFftSize / 2 - 1);
    }
    return true;
}

} // namespace hc::audio
