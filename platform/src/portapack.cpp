/*
 * Host replacement for firmware/application/portapack.cpp.
 *
 * The upstream file owns the global PortaPack objects (LCD, I2C/SPI buses, the
 * Si5351 clock generator, the clock manager, the RX/TX models, the USB serial
 * object) and the boot/shutdown sequence. On the desktop:
 *
 *   - the LCD is the host RGB565 surface (host_lcd.cpp), so the ILI9341 driver
 *     works unchanged;
 *   - the buses (i2c0, ssp1) exist so class members compile, but their driver
 *     back-ends are inert;
 *   - the clock generator / clock manager are host seams (hardware_stubs.cpp);
 *   - audio is routed to a host codec object that will feed SDL/ALSA once the
 *     baseband sample stream exists (stage 4). Today it is a silent sink.
 *
 * Nothing here pretends to be HackRF hardware: the radio objects are the same
 * ones the UI always used, but their implementations are the thin SdrDevice
 * seam, not silicon drivers.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "portapack.hpp"

#include "portapack_hal.hpp"
#include "portapack_dma.hpp"
#include "portapack_shared_memory.hpp"

#include "audio.hpp"
#include "clock_manager.hpp"
#include "host_audio.hpp"
#include "event_m0.hpp"
#include "i2cdevmanager.hpp"
#include "irq_controls.hpp"
#include "rtc_time.hpp"
#include "sd_card.hpp"
#include "touch.hpp"

#include "i2c_pp.hpp"
#include "spi_pp.hpp"

#include "ch.h"

#include <cstdint>
#include <string>

/* Bus drivers. The real firmware gets these from the ChibiOS LPC43xx low-level
 * drivers; on the host they are plain objects because the I2C/SPI transfers are
 * no-ops (see the shim in platform/include/hal.h). */
I2CDriver I2CD0;
SPIDriver SPID2;

namespace portapack {

/* The upstream IO object lives in host_lcd.cpp (it is the framebuffer bus). */

const char* init_error = nullptr;

lcd::ILI9341 display;

I2C i2c0(&I2CD0);
SPI ssp1(&SPID2);
portapack::USBSerial usb_serial;

si5351::Si5351 clock_generator{i2c0, 0x60};
ClockManager clock_manager{i2c0, clock_generator};

ReceiverModel receiver_model;
TransmitterModel transmitter_model;

portapack::BacklightOnOff backlight_on_off;
portapack::BacklightCAT4004 backlight_cat4004;

uint32_t bl_tick_counter{0};
uint16_t touch_threshold{32};

bool antenna_bias{false};

void set_antenna_bias(const bool v) {
    antenna_bias = v;
}

bool get_antenna_bias() {
    return antenna_bias;
}

Backlight* backlight() {
    /* Use the simple on/off backlight: the CAT4004 driver bit-bangs a brightness
     * pulse train through io.lcd_backlight(), which has no meaning on a host. */
    return &backlight_on_off;
}

/*
 * Host audio codec.
 *
 * The upstream codec classes (WM8731/AK4951) drive real I2S codec chips over
 * I2C. On the desktop the sample stream will be handed to the host audio device
 * instead, so the codec object is just a sink that records gain state and
 * reports sensible ranges. The actual PCM push to SDL/ALSA is added in stage 4
 * together with the RX/TX sample path; here it stays silent.
 */
namespace {

class HostAudioCodec : public audio::Codec {
   public:
    std::string name() const override {
        return "Host";
    }

    bool reset() override {
        return true;
    }

    void init() override {}

    void speaker_enable() override {}
    void speaker_disable() override {}

    bool speaker_disable_supported() const override {
        /* Report support so the navigation bar shows the "disable speaker"
         * toggle. The host has a single output, so the firmware's separate
         * speaker/headphone lines are collapsed: config_speaker_disable() mutes
         * the host output too (see input_controls.cpp). */
        return true;
    }

    void headphone_enable() override {}
    void headphone_disable() override {}

    volume_range_t headphone_gain_range() const override {
        return {volume_t::decibel(-73), volume_t::decibel(+6)};
    }

    bool set_headphone_volume(const volume_t volume) override {
        headphone_volume = volume;
        /* The host sound card replaces the codec's analogue gain: forward the
         * apps' volume (games set it through audio::headphone::set_volume) to
         * the SDL renderer, which scales the beep amplitude. */
        host_audio::set_volume_db(volume.decibel());
        return true;
    }

    void microphone_enable(int8_t alc_mode, bool mic_to_HP_enabled) override {
        (void)alc_mode;
        (void)mic_to_HP_enabled;
    }
    void microphone_disable() override {}

    void microphone_to_HP_enable() override {}
    void microphone_to_HP_disable() override {}

    size_t reg_count() const override {
        return 0;
    }
    size_t reg_bits() const override {
        return 0;
    }
    uint32_t reg_read(const size_t register_number) override {
        (void)register_number;
        return 0;
    }
    void reg_write(const size_t register_number, const uint32_t value) override {
        (void)register_number;
        (void)value;
    }

   private:
    volume_t headphone_volume{volume_t::decibel(-20)};
};

HostAudioCodec host_audio_codec;

}  // namespace

init_status_t init() {
    /* Bring up the parts the desktop actually has, in the same order the
     * upstream boot code uses, but without any silicon bring-up. */

    portapack::persistent_memory::cache::init();

    portapack::display.init();
    portapack::backlight()->on();

    rtc_time::dst_init();

    touch_threshold = portapack::persistent_memory::touchscreen_threshold();

    usb_serial.initialize();

    /* controls_init() is deliberately NOT called here: it records chThdSelf()
     * as the switch/encoder event target, so it must run on the UI thread (see
     * platform/src/main.cpp). */

    clock_manager.set_reference_ppb(portapack::persistent_memory::correction_ppb());

    radio::init();

    sdcStart(&SDCD1, nullptr);
    sd_card::poll_inserted();

    audio::init(&host_audio_codec);

    i2cdev::I2CDevManager::init();

    return init_status_t::INIT_SUCCESS;
}

void shutdown(const bool leave_screen_on, const bool slow_clock) {
    (void)slow_clock;

    host_audio::shutdown();

    if (!leave_screen_on) {
        backlight()->off();
        display.shutdown();
    }

    receiver_model.disable();
    transmitter_model.disable();
    audio::shutdown();
}

void setEventDispatcherToUSBSerial(EventDispatcher* evt) {
    usb_serial.setEventDispatcher(evt);
}

bool async_tx_enabled = false;  // this is for serial tx things, globally

}  // namespace portapack
