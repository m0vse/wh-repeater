#pragma once

#include <mutex>

namespace whrepeater {

std::mutex& sharedI2cBusMutex();

} // namespace whrepeater
