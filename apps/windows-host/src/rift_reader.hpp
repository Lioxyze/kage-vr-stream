#pragma once

#include <atomic>
#include <filesystem>

namespace kage::host {
// Blocks until `stop`: reads the Rift CV1 and streams its poses to the KAGE driver.
void runRiftReader(const std::atomic<bool>& stop, const std::filesystem::path& dataDir);
}  // namespace kage::host
