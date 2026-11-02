// libFuzzer target: arbitrary bytes fed to every wire-protocol decoder.

#include <cstdint>
#include <string_view>

#include "fuzz_inputs.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    atlas::fuzz::decode_protocol(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
