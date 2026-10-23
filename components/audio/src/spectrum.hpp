#pragma once

// The spectrum of the player (media_spectrum): the audio task feeds what it plays into a ring,
// the screen's task takes the latest samples from it and transforms them.

#include <cstddef>
#include <cstdint>

namespace hc::audio::detail {

// The ring, before the first file of the player (play_media); false without memory for it.
bool spectrum_prepare();
// From the audio task: the samples of a file of the player before the volume, at `rate`.
void spectrum_feed(const int16_t *samples, size_t count, uint32_t rate);
// nothing plays: media_spectrum() says false until the next feed
void spectrum_clear();

} // namespace hc::audio::detail
