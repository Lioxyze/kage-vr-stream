// USB (cable) mode: adb only forwards TCP, KAGE speaks UDP. Both ends run a bridge
// that carries the UDP datagrams of ports 47800..47802 inside one TCP stream
// (frame = u8 port index, u16 length, payload) over `adb reverse tcp:47810 tcp:47810`.
// Packets stay end-to-end encrypted; the bridge only relays them, on loopback only.
#pragma once

#include <atomic>
#include <cstdint>

namespace kage::net {
inline constexpr uint16_t kUsbBridgePort = 47810;
// PC side (kage-host): listens on 127.0.0.1:47810, relays to the local UDP ports.
void runUsbBridgeHost(const std::atomic<bool>& stop);
// Headset side: binds UDP 127.0.0.1:47800..47802 and dials 127.0.0.1:47810 (adb).
// `connected` reflects whether the tunnel to the PC is up.
void runUsbBridgeClient(const std::atomic<bool>& stop, std::atomic<bool>& connected);
}  // namespace kage::net
