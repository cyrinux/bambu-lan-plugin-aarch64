#pragma once

// The slicer accepts a plugin whose version shares its first 8 characters
// ("02.09.01") with the version it expects. The build number is set high so
// the slicer's plugin update check never offers to replace us with Bambu's
// x86-64 build. Both values are maintained by scripts/upstream.sh.
#define BBL_STUDIO_PLUGIN_VERSION "02.09.01.99"  // Bambu Studio: SLIC3R_VERSION
#define BBL_ORCA_PLUGIN_VERSION "02.03.00.99"    // OrcaSlicer: latest AVAILABLE_NETWORK_VERSIONS

#ifdef BBL_TARGET_ORCA
#define BBL_LAN_PLUGIN_VERSION BBL_ORCA_PLUGIN_VERSION
#define BBL_LAN_PLUGIN_TARGET "orca"
#else
#define BBL_LAN_PLUGIN_VERSION BBL_STUDIO_PLUGIN_VERSION
#define BBL_LAN_PLUGIN_TARGET "studio"
#endif

#ifndef BBL_LAN_PLUGIN_GIT
#define BBL_LAN_PLUGIN_GIT "unknown"
#endif
