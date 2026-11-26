// zodiac::platform::is_elevated and restart_elevated on POSIX systems (contract in platform/elevation.hpp): root is the only
// administrator, and there is no portable way to become it from a running program.
#include <platform/elevation.hpp>
#include <unistd.h>

namespace zodiac::platform {
bool is_elevated() noexcept
{
static const bool elevated=geteuid()==0;
return elevated;
}

Elevation restart_elevated() noexcept
{
return is_elevated()?Elevation::already:Elevation::failed;
}
}
