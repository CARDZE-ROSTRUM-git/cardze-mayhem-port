/*
 * Host battery reporting for the desktop PortaPack.
 *
 * The firmware reads its battery from an I2C gauge chip; the desktop has none,
 * so platform/src/host_battery.cpp replaces firmware/common/battery.cpp and
 * reports the host machine's own battery (a laptop, a Cyberdeck, ...) read from
 * /sys/class/power_supply. This header exposes the periodic push used by the
 * ~1 kHz control tick.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */
#ifndef __HOST_BATTERY_HPP__
#define __HOST_BATTERY_HPP__

namespace host_battery {

/* Pushes a BatteryStateMessage to the UI when the host's power state changed
 * (and once at startup). Called from host_controls_tick(); cheap, it only reads
 * sysfs about once a second. */
void tick();

}  // namespace host_battery

#endif /* __HOST_BATTERY_HPP__ */
