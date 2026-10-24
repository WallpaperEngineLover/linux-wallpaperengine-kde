#pragma once

#include <algorithm>
#include <chrono>
#include <utility>

namespace WallpaperEngine::Application {
/** WE's frame limiter (main loop sub_140110630): a one second budget shared by fps frames instead of a fixed 1/fps
 * sleep. Each frame sleeps its share of what is left of the budget minus the time it already worked, so a frame that
 * ran long or a sleep that came back short is made up by the frames after it. The window starts over when the budget
 * runs out or after fps frames */
class FramePacer {
public:
    using Duration = std::chrono::nanoseconds;

    struct Stats {
	int frames = 0;
	int late = 0;
	Duration work {}, workMax {};
	Duration asked {}, slept {}, overshootMax {};
    };

    /** work: time since the previous frame finished sleeping. Returns how long to sleep now, never negative. fixed
     * sleeps 1/fps minus the work of that frame alone, nothing is made up later */
    Duration beginSleep (Duration work, int fps, bool fixed = false) {
	fps = std::max (1, fps);

	if (fixed) {
	    const auto asked = std::max (Duration::zero (), WINDOW / fps - work);

	    this->m_stats.frames++;
	    this->m_stats.late += asked == Duration::zero () ? 1 : 0;
	    this->m_stats.work += work;
	    this->m_stats.workMax = std::max (this->m_stats.workMax, work);
	    this->m_stats.asked += asked;
	    this->m_asked = asked;

	    return asked;
	}

	this->m_budget -= work;

	if (this->m_budget < Duration::zero () || this->m_index >= fps - 1) {
	    this->m_budget = WINDOW;
	    this->m_index = 0;
	} else {
	    this->m_index++;
	}

	const auto asked = std::max (Duration::zero (), this->m_budget / (fps - this->m_index) - work);

	this->m_stats.frames++;
	this->m_stats.late += asked == Duration::zero () ? 1 : 0;
	this->m_stats.work += work;
	this->m_stats.workMax = std::max (this->m_stats.workMax, work);
	this->m_stats.asked += asked;
	this->m_asked = asked;

	return asked;
    }

    void endSleep (Duration slept) {
	this->m_budget -= slept;
	this->m_stats.slept += slept;
	this->m_stats.overshootMax = std::max (this->m_stats.overshootMax, slept - this->m_asked);
    }

    Stats takeStats () { return std::exchange (this->m_stats, Stats {}); }

private:
    static constexpr Duration WINDOW = std::chrono::seconds (1);

    Duration m_budget = WINDOW;
    int m_index = 0;
    Duration m_asked {};
    Stats m_stats;
};
}
