#pragma once

#include <string>

namespace WallpaperEngine::Audio {
/** WE's audioinputdevice, audioinputvolume and audioinputthreshold */
struct AudioInput {
    /** a source (microphone, line in, a monitor) by name or description, a sink by name for its monitor; empty or
     * "default" is the default sink's monitor */
    std::string device = "default";
    /** 50 is neutral, the spectrum is scaled by volume * 0.02 */
    int volume = 50;
    /** packets whose loudest sample stays under threshold * 0.001 count as silence, 0 is off */
    float threshold = 0.0f;
};
}
