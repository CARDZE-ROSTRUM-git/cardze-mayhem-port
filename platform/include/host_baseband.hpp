/*
 * Host emulation of the M4 baseband core's data side (capture/replay FIFOs).
 *
 * On the device the M4 runs a per-image processor (proc_capture, proc_replay,
 * proc_*_audio, ...). Two of its responsibilities are visible to the M0
 * application side and must exist on the desktop or that side dereferences
 * null pointers:
 *
 *   - Capture: the M4 allocates the StreamBuffer ring (StreamInput) and, as
 *     samples arrive, fills buffers and pushes them to the application. The
 *     RecordView's CaptureThread pulls full buffers from config->fifo_buffers_full
 *     and hands empty ones back through config->fifo_buffers_empty.
 *   - Replay: the mirror image (StreamOutput), where the application fills
 *     buffers and the M4 consumes them.
 *
 * Without an M4 the RecordView's CaptureThread sees config->fifo_buffers_full
 * and config->fifo_buffers_empty equal to nullptr and crashes in
 * BufferExchange::get(). This module plays the M4's part: it creates the ring
 * on a CaptureConfig/ReplayConfig command, feeds it from the SDR backend, and
 * wakes the application thread exactly where creg::m4txevent would have fired
 * the doorbell interrupt.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __HOST_BASEBAND_HPP__
#define __HOST_BASEBAND_HPP__

struct Message;

namespace host_baseband {

/* Which baseband image the application last started (m4_init()). The host has
 * no M4, but the data plane still has to know whether a CaptureConfig refers to
 * the capture ring (proc_capture) or to the audio output ring (proc_*_audio):
 * both images receive the same message, and only the *running* image tells which
 * ring the RecordView's CaptureThread drains. Routing by leftover audio state
 * instead is what blocked a capture recording after the Audio app had been used.
 *
 * The transmit images are tracked for the same reason: proc_replay and
 * proc_audiotx both consume a ReplayConfig, but they fill the TX ring with IQ or
 * with an FM-modulated tone respectively, and the host producer has to run the
 * matching generator. */
enum class Image { Unknown, Capture, Audio, Replay, AudioTX, Other };

/* Records the running baseband image (called from m4_init()). */
void set_image(Image image);

/* Learns the RX baseband rate for images that configure the radio through
 * ReceiverModel instead of a SampleRateConfig (see radio::set_baseband_rate).
 * Called from ClockManager::set_sampling_frequency() on the host. Ignored for
 * images that own another data plane. */
void set_baseband_rate(uint32_t rate);

/* Handles the baseband command messages that carry data-plane state
 * (CaptureConfig, ReplayConfig, SampleRateConfig). Called from the command
 * channel consumer for every message; other ids are ignored. */
void handle_message(const Message* message);

/* Starts the baseband producer thread (the host M4 stand-in). Idempotent. */
void start();

/* Tears down any running producer (used at process exit). */
void shutdown();

}  // namespace host_baseband

#endif /* __HOST_BASEBAND_HPP__ */
