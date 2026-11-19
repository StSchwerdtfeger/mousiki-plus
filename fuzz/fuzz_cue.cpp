// libFuzzer target: see fuzz/README.md
#include "fuzz_cases.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    muisc::fuzz::cue_case(data, size);
    return 0;
}
