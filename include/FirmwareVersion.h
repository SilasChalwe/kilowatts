/**
 * @file FirmwareVersion.h
 * @brief The single firmware version string shared by both Kilowatts roles.
 *
 * Read once at startup and carried in the commissioning identity report
 * (see CommissioningPackets::IdentityReportPacket) so Central knows which
 * firmware build a Node is running. Bump this whenever a change to the
 * commissioning wire protocol, persisted configuration schema, or planning
 * pipeline ships — never invented per-Node, never left to drift between
 * central/smart builds.
 *
 * Kept short (plain semver, no suffix) since it travels inside
 * CommissioningPackets::IdentityReportPacket::firmwareVersion[12], a
 * fixed-size ESP-NOW wire field — must always fit in 11 characters plus
 * the terminating null.
 */

#ifndef KILOWATTS_FIRMWARE_VERSION_H
#define KILOWATTS_FIRMWARE_VERSION_H

namespace kilowatts {

constexpr const char* KILOWATTS_FIRMWARE_VERSION = "0.2.0";

} // namespace kilowatts

#endif // KILOWATTS_FIRMWARE_VERSION_H
