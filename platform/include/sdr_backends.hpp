/*
 * Private bridge between the SDR device facade (sdr_device.cpp) and the
 * individual backends. Each backend translation unit (sdr_rtlsdr.cpp,
 * sdr_pluto.cpp) implements the two hooks for its driver; the facade only
 * calls the ones whose feature macro (HOST_HAVE_RTLSDR / HOST_HAVE_PLUTO) was
 * set by the build, so a build without a backend simply never references it.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#ifndef __HOST_SDR_BACKENDS_HPP__
#define __HOST_SDR_BACKENDS_HPP__

#include "sdr_device.hpp"

#include <memory>
#include <string>
#include <vector>

namespace sdr {

/* RTL2832U (librtlsdr). Defined in sdr_rtlsdr.cpp. */
std::vector<DeviceInfo> rtlsdr_enumerate();
std::unique_ptr<Device> rtlsdr_open(const std::string& args);

/* ADALM-Pluto / Zynq7010 (libiio). Defined in sdr_pluto.cpp. */
std::vector<DeviceInfo> pluto_enumerate();
std::unique_ptr<Device> pluto_open(const std::string& args);

}  // namespace sdr

#endif /*__HOST_SDR_BACKENDS_HPP__*/
