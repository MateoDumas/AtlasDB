#pragma once

#include <string>
#include <string_view>

#include "common/value.h"

namespace atlas {

// Rows are stored as a u16 value count followed by tagged values.
std::string encode_row(const Row& row);
Row decode_row(std::string_view bytes);

}  // namespace atlas
