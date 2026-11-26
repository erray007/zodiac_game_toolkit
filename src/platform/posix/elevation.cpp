// zodiac::platform::is_elevated, restart_elevated and wait_for_restart on POSIX systems (contract in platform/elevation.hpp): root is
// the only administrator, and there is no portable way to become it from a running program.
#include <platform/elevation.hpp>
#include <unistd.h>

namespace zodiac::platform {
bool is_elevated() noexcept
{
static const bool elevated=geteuid()==0;
return elevated;
}

// True when already root (the owner's ruling: every platform answers true for an elevated process), failed otherwise.
Elevation restart_elevated() noexcept
{
return is_elevated()?Elevation::already:Elevation::failed;
}

// No process here ever restarts itself, so there is no guard to wait for or to give back.
void wait_for_restart() noexcept
{
}

void release_held_restart_guard() noexcept
{
}
}
