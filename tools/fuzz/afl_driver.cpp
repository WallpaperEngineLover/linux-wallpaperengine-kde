// Lets AFL++ run the libFuzzer entry points of tools/fuzz/*.cpp: the forkserver starts after the sanitizers are
// set up, every input is one fork. The target is built with clang's trace-pc-guard coverage and afl-compiler-rt.
#include <cstddef>
#include <cstdint>
#include <sys/prctl.h>
#include <unistd.h>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size);
extern "C" void __afl_manual_init ();

int main () {
    // AFL++ wants ASan to abort; a non dumpable process never reaches the host's core handler (systemd-coredump)
    prctl (PR_SET_DUMPABLE, 0);
    __afl_manual_init ();

    std::vector<uint8_t> input;
    uint8_t chunk[65536];
    ssize_t got;

    while ((got = read (0, chunk, sizeof (chunk))) > 0) {
	input.insert (input.end (), chunk, chunk + got);
    }

    LLVMFuzzerTestOneInput (input.data (), input.size ());
    return 0;
}
