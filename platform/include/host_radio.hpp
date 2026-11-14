/*
 * Read-only access to the last radio configuration recorded by the host's
 * radio:: seam (see platform/src/hardware_stubs.cpp).
 *
 * The baseband data-plane emulation (platform/src/host_baseband.cpp) needs the
 * tuning frequency and baseband rate the UI has asked for, so it can open and
 * configure the SDR backend without reaching into the radio seam's internals.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __HOST_RADIO_HPP__
#define __HOST_RADIO_HPP__

#include <cstdint>

namespace host_radio {

/* Last frequency passed to radio::set_tuning_frequency(), in Hz. */
uint64_t tuning_frequency();

/* Last rate passed to radio::set_baseband_rate(), in Hz (0 if none). */
uint32_t baseband_rate();

/*
 * Last gain/bandwidth state recorded by the radio:: seam.
 *
 * The firmware's receiver/transmitter models call radio::set_lna_gain(),
 * set_vga_gain(), set_rf_amp(), set_baseband_filter_bandwidth_*() and
 * set_antenna_bias(); on the device those program the HackRF front-end chips.
 * The host has no such chips, so the seam only caches the values. The host
 * baseband core reads them back here so it can program the real SDR backend
 * instead of ignoring the application's gain and filter choices.
 *
 * lna/vga are the two HackRF receive gain stages in dB (the UI exposes both);
 * rf_amp is the +14 dB front-end amplifier switch; baseband_filter_bandwidth()
 * is the analog filter width in Hz (0 if the model never asked).
 */
int lna_gain_db();
int vga_gain_db();
bool rf_amp();

uint32_t baseband_filter_bandwidth();

/* Antenna bias tee / antenna power, as last requested (transmit-only on the
 * device; the host maps it to the backend's bias-tee control where supported).
 */
bool antenna_bias();

/* True when the radio:: seam was last told to transmit (rf::Direction::Transmit).
 * The host has no transmit path, so the baseband producer uses this only to know
 * which direction the application expects. */
bool transmitting();

/* Last TX gain in dB passed to radio::set_tx_gain() (HackRF 0..47 dB on the
 * device; mapped onto the backend's TX gain span by the host baseband). */
int tx_gain_db();

}  // namespace host_radio

#endif /* __HOST_RADIO_HPP__ */
