#pragma once

// The slicer accepts a plugin whose version shares its first 8 characters
// ("02.09.01") with SLIC3R_VERSION. The build number is set high so the
// slicer's plugin update check never offers to replace us with Bambu's x86-64
// build.
#define BBL_LAN_PLUGIN_VERSION "02.09.01.99"

#ifndef BBL_LAN_PLUGIN_GIT
#define BBL_LAN_PLUGIN_GIT "unknown"
#endif
