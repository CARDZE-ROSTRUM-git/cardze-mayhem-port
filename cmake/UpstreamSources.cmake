# Generated from upstream firmware/application/CMakeLists.txt; hardware layers removed.
set(MAYHEM_UPSTREAM_SOURCES
    # M4-side stream rings, reused verbatim on the host by the baseband data
    # plane emulation (platform/src/host_baseband.cpp). On the device only the
    # M4 core compiles these; here they run so that the application-side
    # CaptureThread/ReplayThread see real FIFOs instead of null pointers.
    ${MAYHEM_DIR}/firmware/baseband/stream_input.cpp
    ${MAYHEM_DIR}/firmware/baseband/stream_output.cpp
    ${MAYHEM_DIR}/firmware/common/adsb.cpp
    ${MAYHEM_DIR}/firmware/common/adsb_frame.cpp
    ${MAYHEM_DIR}/firmware/common/ais_baseband.cpp
    ${MAYHEM_DIR}/firmware/common/ais_packet.cpp
    ${MAYHEM_DIR}/firmware/common/bch_code.cpp
    ${MAYHEM_DIR}/firmware/common/buffer.cpp
    # NOTE: firmware/common/buffer_exchange.cpp is deliberately NOT listed. Its
    # get() parks the consumer on the ChibiOS scheduler and relies on the M4 IRQ
    # to wake it; on the host the producer runs on an unrelated std::thread, so a
    # lost wake-up (or a terminate request) leaves the consumer asleep and
    # RecordView::stop()'s join never returns. The behaviour-preserving,
    # poll-and-check-terminate host replacement is
    # platform/src/host_buffer_exchange.cpp.
    ${MAYHEM_DIR}/firmware/common/ert_packet.cpp
    ${MAYHEM_DIR}/firmware/common/event.cpp
    # NOTE: firmware/common/gcc.cpp is deliberately NOT listed. It provides
    # freestanding definitions (its own __dso_handle, __cxa_pure_virtual,
    # __verbose_terminate_handler) that clash with the host C++ runtime, which
    # supplies them via crtbeginS.o/libstdc++. Including it fails the link with
    # a duplicate __dso_handle.
    ${MAYHEM_DIR}/firmware/common/lfsr_random.cpp
    ${MAYHEM_DIR}/firmware/common/manchester.cpp
    ${MAYHEM_DIR}/firmware/common/message_queue.cpp
    ${MAYHEM_DIR}/firmware/common/morse.cpp
    ${MAYHEM_DIR}/firmware/common/png_writer.cpp
    ${MAYHEM_DIR}/firmware/common/pocsag.cpp
    ${MAYHEM_DIR}/firmware/common/pocsag_packet.cpp
    ${MAYHEM_DIR}/firmware/common/aprs_packet.cpp
    ${MAYHEM_DIR}/firmware/common/portapack_persistent_memory.cpp
    ${MAYHEM_DIR}/firmware/common/portapack_shared_memory.cpp
    ${MAYHEM_DIR}/firmware/common/sonde_packet.cpp
    ${MAYHEM_DIR}/firmware/common/tpms_packet.cpp
    ${MAYHEM_DIR}/firmware/common/ui.cpp
    ${MAYHEM_DIR}/firmware/common/ui_focus.cpp
    ${MAYHEM_DIR}/firmware/common/ui_painter.cpp
    ${MAYHEM_DIR}/firmware/common/ui_text.cpp
    ${MAYHEM_DIR}/firmware/common/ui_widget.cpp
    ${MAYHEM_DIR}/firmware/common/ui_language.cpp
    ${MAYHEM_DIR}/firmware/common/utility.cpp
    # NOTE: firmware/common/battery.cpp is deliberately NOT listed. It reports
    # the state of the MAX17055/ADS1110 I2C gauge chips through I2CDevManager,
    # which the host does not have, so isDetected() is always false and the
    # status bar never shows a battery. The host replacement reads the desktop's
    # own battery (e.g. a laptop / Cyberdeck) from /sys/class/power_supply and is
    # platform/src/host_battery.cpp.
    ${MAYHEM_DIR}/firmware/common/performance_counter.cpp
    ${MAYHEM_DIR}/firmware/common/bmpfile.cpp
    ${MAYHEM_DIR}/firmware/common/gpio.cpp
    ${MAYHEM_DIR}/firmware/application/app_settings.cpp
    ${MAYHEM_DIR}/firmware/application/baseband_api.cpp
    ${MAYHEM_DIR}/firmware/application/capture_thread.cpp
    ${MAYHEM_DIR}/firmware/application/database.cpp
    ${MAYHEM_DIR}/firmware/application/gradient.cpp
    ${MAYHEM_DIR}/firmware/application/event_m0.cpp
    # NOTE: firmware/application/file_reader.cpp is deliberately NOT listed.
    # Its split_string() indexes one past the end of the string_view when the
    # input ends with the delimiter, which aborts on the host runtime; the
    # behaviour-preserving host replacement is
    # platform/src/host_file_reader.cpp.
    ${MAYHEM_DIR}/firmware/application/file.cpp
    ${MAYHEM_DIR}/firmware/application/file_path.cpp
    ${MAYHEM_DIR}/firmware/application/freqman_db.cpp
    ${MAYHEM_DIR}/firmware/application/freqman.cpp
    ${MAYHEM_DIR}/firmware/application/io_convert.cpp
    ${MAYHEM_DIR}/firmware/application/lcd_tuning_file.cpp
    ${MAYHEM_DIR}/firmware/application/io_file.cpp
    ${MAYHEM_DIR}/firmware/application/io_wave.cpp
    ${MAYHEM_DIR}/firmware/application/iq_trim.cpp
    ${MAYHEM_DIR}/firmware/application/keeloq_common.cpp
    ${MAYHEM_DIR}/firmware/application/keeloq_file.cpp
    ${MAYHEM_DIR}/firmware/application/keeloq_keystore.cpp
    ${MAYHEM_DIR}/firmware/application/log_file.cpp
    ${MAYHEM_DIR}/firmware/application/metadata_file.cpp
    ${MAYHEM_DIR}/firmware/application/flipper_subfile.cpp
    ${MAYHEM_DIR}/firmware/application/qrcodegen.cpp
    ${MAYHEM_DIR}/firmware/application/receiver_model.cpp
    ${MAYHEM_DIR}/firmware/application/spec_an_sweep.cpp
    ${MAYHEM_DIR}/firmware/application/recent_entries.cpp
    ${MAYHEM_DIR}/firmware/application/replay_thread.cpp
    ${MAYHEM_DIR}/firmware/application/serializer.cpp
    ${MAYHEM_DIR}/firmware/application/spectrum_color_lut.cpp
    ${MAYHEM_DIR}/firmware/application/string_format.cpp
    ${MAYHEM_DIR}/firmware/application/theme.cpp
    ${MAYHEM_DIR}/firmware/application/touch.cpp
    ${MAYHEM_DIR}/firmware/application/tone_key.cpp
    ${MAYHEM_DIR}/firmware/application/transmitter_model.cpp
    ${MAYHEM_DIR}/firmware/application/tuning.cpp
    ${MAYHEM_DIR}/firmware/application/ook_file.cpp
    ${MAYHEM_DIR}/firmware/application/ui_baseband_stats_view.cpp
    ${MAYHEM_DIR}/firmware/application/ui_navigation.cpp
    ${MAYHEM_DIR}/firmware/application/ui_notifications.cpp
    ${MAYHEM_DIR}/firmware/application/ui_record_view.cpp
    ${MAYHEM_DIR}/firmware/application/ui_sd_card_status_view.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_alphanum.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_audio.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_channel.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_font_fixed_5x8.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_font_fixed_8x16.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_geomap.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_qrcode.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_menu.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_btngrid.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_receiver.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_rssi.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_freqlist.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_spectrum.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_tabview.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_textentry.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_tone_key.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_transmitter.cpp
    ${MAYHEM_DIR}/firmware/application/ui/ui_bmpview.cpp
    ${MAYHEM_DIR}/firmware/application/apps/analog_audio_app.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ble_rx_app.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ble_tx_app.cpp
    ${MAYHEM_DIR}/firmware/application/apps/capture_app.cpp
    ${MAYHEM_DIR}/firmware/application/apps/pocsag_app.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_about_simple.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_battinfo.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_bmp_file_viewer.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_btle_rx.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_debug.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_debug_max17055.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_dfu_menu.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_encoders.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_external_module_view.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_fileman.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_flash_utility.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_freqman.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_iq_trim.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_looking_glass_app.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_mictx.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_modemsetup.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_playlist.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_rds.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_recon_settings.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_recon.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_search.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_settings.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_sonde.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_ss_viewer.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_standalone_view.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_subghzd.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_text_editor.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_touch_calibration.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_touchtunes.cpp
    ${MAYHEM_DIR}/firmware/application/apps/ui_weatherstation.cpp
    ${MAYHEM_DIR}/firmware/application/protocols/aprs.cpp
    ${MAYHEM_DIR}/firmware/application/protocols/ax25.cpp
    ${MAYHEM_DIR}/firmware/application/protocols/dcs.cpp
    ${MAYHEM_DIR}/firmware/application/protocols/encoders.cpp
    ${MAYHEM_DIR}/firmware/application/protocols/modems.cpp
    ${MAYHEM_DIR}/firmware/application/protocols/rds.cpp
    ${MAYHEM_DIR}/firmware/application/ui_sd_card_debug.cpp
    ${MAYHEM_DIR}/firmware/application/view_factory_base.cpp

    # --- Portable upstream layers reused verbatim on the host ---
    # The LCD driver and backlight draw through portapack::IO, which is
    # redirected to the RGB565 surface by the host shim; everything else here is
    # platform-neutral logic. The HackRF radio periphery (CPLD/JTAG/si5351/
    # MAX283x/RFFC507x) is NOT built: the radio layer is a thin host seam that
    # will be backed by SdrDevice (PlutoSDR via libiio / RTL-SDR via librtlsdr).
    ${MAYHEM_DIR}/firmware/common/lcd_ili9341.cpp
    ${MAYHEM_DIR}/firmware/common/backlight.cpp
    ${MAYHEM_DIR}/firmware/common/i2cdevmanager.cpp
    ${MAYHEM_DIR}/firmware/common/i2cdev_max17055.cpp
    ${MAYHEM_DIR}/firmware/common/i2cdev_ads1110.cpp
    ${MAYHEM_DIR}/firmware/common/i2cdev_ppmod.cpp
    # Environmental sensors: instantiated by i2cdevmanager, so their vtables are
    # referenced even though the host has no such chips (calls are inert).
    ${MAYHEM_DIR}/firmware/common/i2cdev_bh1750.cpp
    ${MAYHEM_DIR}/firmware/common/i2cdev_bmx280.cpp
    ${MAYHEM_DIR}/firmware/common/i2cdev_sht3x.cpp
    ${MAYHEM_DIR}/firmware/common/i2cdev_sht4x.cpp
    # Audio codec drivers (WM8731/AK4951) and the Si5351 clock generator are kept
    # so their class methods/vtables resolve; the host codec is a separate sink.
    ${MAYHEM_DIR}/firmware/common/wm8731.cpp
    ${MAYHEM_DIR}/firmware/common/ak4951.cpp
    ${MAYHEM_DIR}/firmware/application/hw/si5351.cpp
    ${MAYHEM_DIR}/firmware/application/audio.cpp
    # Upstream I2C bit-bang wrapper and the GPDMA controller used by audio.cpp.
    ${MAYHEM_DIR}/firmware/common/i2c_pp.cpp
    ${MAYHEM_DIR}/firmware/common/gpdma.cpp
    ${MAYHEM_DIR}/firmware/common/chibios_cpp.cpp
    ${MAYHEM_DIR}/firmware/application/rtc_time.cpp
    ${MAYHEM_DIR}/firmware/application/sd_card.cpp
    ${MAYHEM_DIR}/firmware/application/usb_serial_asyncmsg.cpp
    ${MAYHEM_DIR}/firmware/application/usb_serial_event.cpp
    ${MAYHEM_DIR}/firmware/application/usb_serial_thread.cpp
    ${MAYHEM_DIR}/firmware/application/hw/debounce.cpp
    ${MAYHEM_DIR}/firmware/application/hw/encoder.cpp

    # --- M4 receive DSP, reused verbatim on the host ---
    # The host baseband core (platform/src/host_baseband.cpp) runs the firmware's
    # own decimation chain and FFT, so the waterfall and capture use the device's
    # arithmetic rather than a reimplementation.
    ${MAYHEM_DIR}/firmware/baseband/dsp_decimate.cpp
    ${MAYHEM_DIR}/firmware/baseband/dsp_demodulate.cpp
    ${MAYHEM_DIR}/firmware/baseband/dsp_hilbert.cpp
    # Modulation core shared by the mic-TX and audio-TX images (proc_mictx /
    # proc_audiotx). The host's TX producer drives it directly instead of
    # reimplementing FM/AM/SSB/DSB, so the arithmetic is the device's own.
    ${MAYHEM_DIR}/firmware/baseband/dsp_modulate.cpp
    ${MAYHEM_DIR}/firmware/baseband/tone_gen.cpp
    ${MAYHEM_DIR}/firmware/baseband/dsp_squelch.cpp
    ${MAYHEM_DIR}/firmware/baseband/fxpt_atan2.cpp
    ${MAYHEM_DIR}/firmware/baseband/audio_compressor.cpp
    ${MAYHEM_DIR}/firmware/common/dsp_iir.cpp
    ${MAYHEM_DIR}/firmware/common/dsp_fir_taps.cpp
)
