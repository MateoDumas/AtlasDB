#pragma once

// The version comes from project(... VERSION ...) in CMakeLists.txt.
#ifndef ATLAS_VERSION
#define ATLAS_VERSION "dev"
#endif

namespace atlas {

inline constexpr const char* VERSION = ATLAS_VERSION;
inline constexpr const char* PRODUCT = "AtlasDB " ATLAS_VERSION;

}  // namespace atlas
