/*
 * Host audio output for the desktop PortaPack.
 *
 * The firmware's games and apps ask for a tone by sending an AudioBeepMessage
 * to the (absent) M4 baseband core: baseband::request_audio_beep(freq,
 * sample_rate, duration_ms) and baseband::request_beep_stop(). On the device the
 * M4 synthesises the beep into the I2S stream that feeds the codec. The desktop
 * has neither an M4 nor an I2S codec, so this module plays the same beep on the
 * host's sound card through SDL audio, which is the whole point of the port.
 *
 * The tone is generated at the audio device's own sample rate (resampled from
 * the requested frequency), so a device that only runs at 48 kHz still produces
 * the correct pitch. The headphone volume the games set through
 * audio::headphone::set_volume() is applied to the amplitude.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __HOST_AUDIO_HPP__
#define __HOST_AUDIO_HPP__

#include <cstddef>
#include <cstdint>

namespace host_audio {

/* Plays a tone of `freq` Hz for `duration_ms` milliseconds. A duration of 0
 * means "until stopped", matching the M4's beep semantics; tick() keeps such a
 * tone fed. The `sample_rate` argument mirrors the firmware call but does not
 * force the device rate: the tone is synthesised at the device's rate. */
void play_beep(uint32_t freq, uint32_t sample_rate, uint32_t duration_ms);

/* Feeds demodulated receive audio (mono, signed 16-bit) produced by the host
 * baseband core at `sample_rate` Hz. This replaces the M4's I2S/DMA audio path:
 * the samples are resampled to the sound card's own rate and queued for the
 * SDL callback, which is what makes the receiver audible. While PCM is queued
 * it takes priority over any beep. */
void write_pcm(const int16_t* samples, size_t count, uint32_t sample_rate);

/* Stops receive audio: drains the PCM queue so the beep synthesiser can take
 * over again. Called when an app leaves an audio mode. */
void stop_pcm();

/* Mutes or unmutes the whole host output (receive audio and beeps alike). This
 * is the desktop's stand-in for the codec's headphone/speaker enable lines: the
 * firmware arbitrates muting in audio::output and finally toggles those lines,
 * so the host codec maps them straight onto this. Muted output emits silence. */
void set_muted(bool muted);

/* Stops the current tone and drops anything still queued. */
void stop();

/* Sets the output attenuation from a decibel value (0 dB = full scale), the way
 * audio::headphone::set_volume() carries it. */
void set_volume_db(int db);

/* Keeps a continuous (duration_ms == 0) tone fed. Called from the ~1 kHz host
 * control tick. Cheap when nothing is playing. */
void tick();

/* Closes the audio device. */
void shutdown();

}  // namespace host_audio

#endif /* __HOST_AUDIO_HPP__ */
