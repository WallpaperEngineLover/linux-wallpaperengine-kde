// headphones_probe [seconds]: prints Detectors::HeadphonesDetector's answer whenever it changes (headphones_reconnect.sh)
#include "WallpaperEngine/Audio/Drivers/Detectors/HeadphonesDetector.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
int main (int argc, char** argv) {
    WallpaperEngine::Audio::Drivers::Detectors::HeadphonesDetector detector;
    const int secs = argc > 1 ? atoi (argv[1]) : 20;
    bool last = !detector.headphones ();
    for (int i = 0; i < secs * 10; i++) {
	if (detector.headphones () != last) {
	    last = detector.headphones ();
	    printf ("%.1f headphones %d\n", i / 10.0, last);
	    fflush (stdout);
	}
	std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
}
