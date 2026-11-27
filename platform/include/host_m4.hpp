/*
 * Host emulation of the M4 baseband core's *processor* side.
 *
 * On the device each baseband image is a self-contained M4 program
 * (firmware/baseband/proc_*.cpp) whose main() builds an EventDispatcher and an
 * image-specific BasebandProcessor. The processor consumes the raw SGPIO IQ
 * stream (buffer_c8_t at the baseband rate) and pushes decoded messages
 * (WeatherDataMessage, SubGhzDDataMessage, APRS packets, ...) into
 * shared_memory.application_queue, where the M0 application drains them.
 *
 * Earlier the desktop port only reimplemented the data planes it needed
 * (capture ring, audio output, spectrum/waterfall) inside host_baseband.cpp and
 * compiled *no* proc_*.cpp at all. Every packet-decoding app was therefore dead:
 * the SDR ran, the waterfall scrolled, but no decoder existed to turn the IQ
 * into packets. Weather was the visible symptom; subghz, POCSAG, ADS-B, AIS, ...
 * were all affected the same way.
 *
 * This module restores the real thing instead of reimplementing it: the
 * firmware's own processor sources are compiled into the desktop binary (with
 * their M4 main() renamed and garbage-collected away) and instantiated here by
 * image tag. host_baseband feeds them the IQ it pulls from the SDR backend, and
 * the command channel (service_baseband_channel) forwards messages to
 * on_message(), exactly as the M4 event loop did on the device.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __HOST_M4_HPP__
#define __HOST_M4_HPP__

#include <cstddef>
#include <cstdint>

#include "spi_image.hpp"

struct Message;

namespace host_m4 {

/* Tears down any running image and, if `tag` names a decoder this build
 * registers, constructs its processor. Called from m4_init() for every image the
 * application loads, so an image without a registered processor (capture,
 * audio, replay, spec-an, ...) leaves the M4 idle and the host's own data planes
 * keep owning the radio. */
void start_image(const portapack::spi_flash::image_tag_t& tag);

/* Destroys the running processor (process exit / image teardown). */
void stop_image();

/* True while a registered decoder is running. host_baseband checks this to
 * decide between the firmware processor and its own generic RX chain. */
bool active();

/* Forwards a command message to the running processor's on_message(). */
void on_message(const Message* message);

/* Feeds one block of interleaved signed 8-bit IQ to the running processor.
 * `rate` is the baseband sample rate the SDR delivered the block at. */
void feed(const int8_t* samples, size_t count, uint32_t rate);

}  // namespace host_m4

#endif /* __HOST_M4_HPP__ */
