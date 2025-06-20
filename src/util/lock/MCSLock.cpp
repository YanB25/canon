#include "util/lock/MCSLock.h"

namespace util
{
thread_local MCSLock::QNode MCSLock::my_node_;
}