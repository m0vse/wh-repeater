#include "whrepeater/i2c_bus_lock.hpp"

namespace whrepeater {

std::mutex& sharedI2cBusMutex()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace whrepeater
