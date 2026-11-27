# The firmware's real M4 receive processors (firmware/baseband/proc_*.cpp),
# compiled into the desktop binary so packet-decoding apps actually decode.
#
# Background: the port used to reimplement only the data planes it needed
# (capture ring, audio sink, waterfall) and compiled no proc_*.cpp at all, so
# every decoder app (weather, subghz, POCSAG, ADS-B, AIS, radiosonde, ...) was
# dead: the SDR ran but nothing turned IQ into packets. This list restores the
# upstream processors verbatim; host_m4.cpp instantiates them by image tag and
# feeds them the SDR IQ stream.
#
# Each proc_*.cpp is a complete M4 program with its own int main(). The main()
# is not what we run (host_m4 builds the processor directly), so it is renamed
# to a unique bb_* symbol and dropped by --gc-sections together with everything
# only it referenced. That is why the target enables -ffunction-sections /
# -fdata-sections and -Wl,--gc-sections (see CMakeLists.txt).

# File basename (firmware/baseband/proc_<name>.cpp) of every registered image.
# host_m4.cpp's factory must map the same image tags to the same processors
# (see make_processor()).
set(MAYHEM_M4_PROC_NAMES
    weather
    subghzd
    subghz_chat
    pocsag2
    flex
    adsbrx
    ais
    aprsrx
    sonde
    tpms
    ert
    acars
    afskrx
    rds_rx
    wmbus
    vdl2
    tetra
    morse
    rtty_rx
    tonedetect
    epirb
    noaaapt_rx
    wefaxrx
    protoview
    btlerx
    nrfrx
    fsk_rx
    ook
    subcar
    # Second wave: apps whose image had no processor registered at all, so
    # their UI opened but nothing decoded. Restored upstream verbatim.
    signal_hunter
    ft8_rx
    sstvrx
    vor_rx
    time_sink
    am_tv
)

set(MAYHEM_M4_PROC_SOURCES)
foreach(_name ${MAYHEM_M4_PROC_NAMES})
    list(APPEND MAYHEM_M4_PROC_SOURCES ${FW}/baseband/proc_${_name}.cpp)
    # Rename the (unused) M4 entry point so the images do not collide and both
    # it and its callees are removed by the linker.
    set_source_files_properties(
        ${FW}/baseband/proc_${_name}.cpp
        PROPERTIES COMPILE_DEFINITIONS "main=bb_${_name}")
endforeach()

# M4 baseband infrastructure shared by the processors.
set(MAYHEM_M4_SUPPORT_SOURCES
    ${FW}/baseband/baseband_processor.cpp
    ${FW}/baseband/channel_decimator.cpp
    ${FW}/baseband/clock_recovery.cpp
    ${FW}/baseband/matched_filter.cpp
    ${FW}/baseband/packet_builder.cpp
    ${FW}/baseband/dsp_goertzel.cpp
    ${FW}/baseband/spectrum_collector.cpp
    ${FW}/baseband/filtered_spectrum_collector.cpp
    ${FW}/baseband/tv_collector.cpp
    ${FW}/baseband/audio_output.cpp
    ${FW}/baseband/audio_input.cpp
    ${FW}/baseband/audio_stats_collector.cpp
    ${FW}/baseband/cc1101_decoder.cpp
    ${FW}/baseband/vdl2_decoder.cpp
    ${FW}/common/dsp_fft.cpp
    ${FW}/common/dsp_sos.cpp
    ${FW}/common/random.cpp
    # FT8: the upstream iterative decoder (Public domain / MIT ft8_lib) plus the
    # PortaPack glue that turns the Costas-correlated audio into messages.
    ${FW}/baseband/ft8_portapack.c
    ${FW}/baseband/ft8_lib/constants.c
    ${FW}/baseband/ft8_lib/crc.c
    ${FW}/baseband/ft8_lib/decode.c
    ${FW}/baseband/ft8_lib/ldpc.c
    ${FW}/baseband/ft8_lib/message.c
    ${FW}/baseband/ft8_lib/text.c
)

set(MAYHEM_M4_SOURCES ${MAYHEM_M4_SUPPORT_SOURCES} ${MAYHEM_M4_PROC_SOURCES})
