#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/PulseAudioPlayingDetector.h"
#include "WallpaperEngine/Render/Drivers/Detectors/FullScreenDetector.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
using WallpaperEngine::Application::ApplicationContext;
using WallpaperEngine::Audio::Drivers::Detectors::PulseAudioPlayingDetector;
using WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector;

namespace {
using Clock = std::chrono::steady_clock;

class FakeFullScreen final : public FullScreenDetector {
public:
    using FullScreenDetector::FullScreenDetector;

    bool fullscreen = false;

    [[nodiscard]] bool anythingFullscreen () const override { return this->fullscreen; }
};

// a PipeWire stack of its own in a temp dir with one null sink, never the desktop's server; the tests skip when the
// programs are missing or it does not come up
class PrivateSoundServer {
public:
    PrivateSoundServer () {
	char pattern[] = "/tmp/lwe-automute-XXXXXX";

	if (mkdtemp (pattern) == nullptr) {
	    return;
	}

	this->m_dir = pattern;
	this->m_env = {
	    "HOME=" + this->m_dir,
	    "PATH=/usr/bin:/usr/sbin:/bin:/sbin",
	    "XDG_RUNTIME_DIR=" + this->m_dir,
	    "PULSE_RUNTIME_PATH=" + this->m_dir,
	    "PIPEWIRE_RUNTIME_DIR=" + this->m_dir,
	    "XDG_CONFIG_HOME=" + this->m_dir + "/config",
	    "XDG_STATE_HOME=" + this->m_dir + "/state",
	    "XDG_DATA_HOME=" + this->m_dir + "/data",
	    "DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent",
	    "ALSA_CONFIG_PATH=/dev/null",
	};

	for (const char* sub : { "config", "state", "data" }) {
	    std::filesystem::create_directories (this->m_dir + "/" + sub);
	}

	for (const char* program : { "pipewire", "wireplumber", "pipewire-pulse" }) {
	    const pid_t pid = this->spawn ({ program }, -1);

	    if (pid < 0) {
		return;
	    }

	    this->m_daemons.push_back (pid);
	    std::this_thread::sleep_for (300ms);
	}

	for (int i = 0; i < 100 && !this->m_ready; i++) {
	    this->m_ready = this->run ({ "pactl", "info" }).second == 0;

	    if (!this->m_ready) {
		std::this_thread::sleep_for (100ms);
	    }
	}

	if (!this->m_ready) {
	    return;
	}

	this->run ({ "pactl", "load-module", "module-null-sink", "sink_name=lwe_automute" });
	this->run ({ "pactl", "set-default-sink", "lwe_automute" });
    }

    ~PrivateSoundServer () {
	this->stopPlayer ();

	for (const pid_t pid : this->m_daemons) {
	    kill (pid, SIGCONT);
	    kill (pid, SIGTERM);
	}

	for (const pid_t pid : this->m_daemons) {
	    waitpid (pid, nullptr, 0);
	}

	if (!this->m_dir.empty ()) {
	    std::error_code ec;
	    std::filesystem::remove_all (this->m_dir, ec);
	}
    }

    PrivateSoundServer (const PrivateSoundServer&) = delete;
    PrivateSoundServer& operator= (const PrivateSoundServer&) = delete;

    [[nodiscard]] bool ready () const { return this->m_ready; }
    [[nodiscard]] std::string address () const { return "unix:" + this->m_dir + "/native"; }
    [[nodiscard]] std::string missingSocket () const { return "unix:" + this->m_dir + "/no-such-socket"; }

    void freeze () { kill (this->m_daemons.back (), SIGSTOP); }
    void thaw () { kill (this->m_daemons.back (), SIGCONT); }

    void startPlayer () {
	this->stopPlayer ();

	const int zero = open ("/dev/zero", O_RDONLY | O_CLOEXEC);
	this->m_player = this->spawn (
	    { "pacat", "--playback", "--raw", "--format=s16le", "--rate=48000", "--channels=2", "--latency-msec=20" },
	    zero
	);
	close (zero);
    }

    void stopPlayer () {
	if (this->m_player > 0) {
	    kill (this->m_player, SIGTERM);
	    waitpid (this->m_player, nullptr, 0);
	    this->m_player = -1;
	}
    }

    int firstSinkInput () {
	const auto listing = this->run ({ "pactl", "list", "short", "sink-inputs" }).first;

	return listing.empty () ? -1 : static_cast<int> (std::strtol (listing.c_str (), nullptr, 10));
    }

    void setMute (int index, bool mute) {
	this->run ({ "pactl", "set-sink-input-mute", std::to_string (index), mute ? "1" : "0" });
    }

private:
    pid_t spawn (const std::vector<std::string>& argv, int input, int output = -1) {
	std::vector<char*> args;
	std::vector<char*> env;

	for (const auto& arg : argv) {
	    args.push_back (const_cast<char*> (arg.c_str ()));
	}

	for (const auto& entry : this->m_env) {
	    env.push_back (const_cast<char*> (entry.c_str ()));
	}

	args.push_back (nullptr);
	env.push_back (nullptr);

	const pid_t pid = fork ();

	if (pid == 0) {
	    // Catch2's fatal signal handlers would print the parent's report if a TERM arrives before the exec
	    for (const int sig : { SIGTERM, SIGINT, SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS }) {
		signal (sig, SIG_DFL);
	    }

	    setpgid (0, 0);

	    const int devnull = open ("/dev/null", O_RDWR);

	    if (devnull < 0) {
		_exit (127);
	    }

	    dup2 (input >= 0 ? input : devnull, STDIN_FILENO);
	    dup2 (output >= 0 ? output : devnull, STDOUT_FILENO);
	    dup2 (devnull, STDERR_FILENO);
	    execvpe (args[0], args.data (), env.data ());
	    _exit (127);
	}

	return pid;
    }

    std::pair<std::string, int> run (const std::vector<std::string>& argv) {
	int fds[2];

	if (pipe (fds) != 0) {
	    return { "", -1 };
	}

	const pid_t pid = this->spawn (argv, -1, fds[1]);
	close (fds[1]);

	std::string out;
	char buffer[512];

	for (ssize_t got; (got = read (fds[0], buffer, sizeof (buffer))) > 0;) {
	    out.append (buffer, static_cast<size_t> (got));
	}

	close (fds[0]);

	int status = 0;
	waitpid (pid, &status, 0);

	return { out, WIFEXITED (status) ? WEXITSTATUS (status) : -1 };
    }

    std::string m_dir;
    std::vector<std::string> m_env;
    std::vector<pid_t> m_daemons;
    pid_t m_player = -1;
    bool m_ready = false;
};

PrivateSoundServer& server () {
    static PrivateSoundServer instance;

    return instance;
}

class EnvOverride {
public:
    EnvOverride (const char* name, const std::string& value) : m_name (name) {
	if (const char* old = getenv (name)) {
	    this->m_old = old;
	    this->m_had = true;
	}

	setenv (name, value.c_str (), 1);
    }

    ~EnvOverride () {
	if (this->m_had) {
	    setenv (this->m_name, this->m_old.c_str (), 1);
	} else {
	    unsetenv (this->m_name);
	}
    }

    EnvOverride (const EnvOverride&) = delete;
    EnvOverride& operator= (const EnvOverride&) = delete;

private:
    const char* m_name;
    std::string m_old;
    bool m_had = false;
};

ApplicationContext makeContext () {
    static const char* argv[] = { "linux-wallpaperengine" };

    return ApplicationContext (1, const_cast<char**> (argv));
}

template <typename Predicate>
bool pumpUntil (PulseAudioPlayingDetector& detector, Predicate&& done, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = Clock::now () + timeout;

    while (Clock::now () < deadline) {
	detector.update ();

	if (done ()) {
	    return true;
	}

	std::this_thread::sleep_for (10ms);
    }

    return false;
}
}

#define REQUIRE_PRIVATE_SERVER()                                                                                       \
    if (!server ().ready ()) {                                                                                         \
	SKIP ("pipewire, wireplumber, pipewire-pulse, pactl and pacat are needed for a private sound server");         \
    }                                                                                                                  \
    EnvOverride pulseServer ("PULSE_SERVER", server ().address ());                                                    \
    server ().stopPlayer ()

TEST_CASE ("automute sees another app's sound start and stop") {
    REQUIRE_PRIVATE_SERVER ();

    auto context = makeContext ();
    context.settings.audio.automute = true;
    FakeFullScreen fullscreen (context);
    PulseAudioPlayingDetector detector (context, fullscreen);

    REQUIRE_FALSE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }, 500ms));

    server ().startPlayer ();
    REQUIRE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }));

    server ().stopPlayer ();
    REQUIRE (pumpUntil (detector, [&] { return !detector.anythingPlaying (); }));
}

TEST_CASE ("automute follows a stream being muted and unmuted") {
    REQUIRE_PRIVATE_SERVER ();

    auto context = makeContext ();
    context.settings.audio.automute = true;
    FakeFullScreen fullscreen (context);
    PulseAudioPlayingDetector detector (context, fullscreen);

    server ().startPlayer ();
    REQUIRE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }));

    const int input = server ().firstSinkInput ();
    REQUIRE (input >= 0);

    server ().setMute (input, true);
    REQUIRE (pumpUntil (detector, [&] { return !detector.anythingPlaying (); }));

    server ().setMute (input, false);
    REQUIRE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }));
}

TEST_CASE ("automute finds a stream that was already playing when the detector started") {
    REQUIRE_PRIVATE_SERVER ();

    server ().startPlayer ();
    for (int i = 0; i < 100 && server ().firstSinkInput () < 0; i++) {
	std::this_thread::sleep_for (50ms);
    }

    auto context = makeContext ();
    context.settings.audio.automute = true;
    FakeFullScreen fullscreen (context);
    PulseAudioPlayingDetector detector (context, fullscreen);

    REQUIRE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }));
}

TEST_CASE ("automute never makes a frame wait for the sound server") {
    REQUIRE_PRIVATE_SERVER ();

    auto context = makeContext ();
    context.settings.audio.automute = true;
    FakeFullScreen fullscreen (context);
    PulseAudioPlayingDetector detector (context, fullscreen);

    // a server that stops answering (busy, suspended, stuck): the query goes out and never comes back. The release
    // timer only keeps a regression from hanging the run, a blocking update() shows up as the worst frame time
    std::atomic<bool> finished = false;
    std::thread release ([&] {
	for (int i = 0; i < 60 && !finished; i++) {
	    std::this_thread::sleep_for (50ms);
	}

	server ().thaw ();
    });

    server ().freeze ();
    server ().startPlayer ();

    Clock::duration worst = 0ms;

    for (int frame = 0; frame < 100; frame++) {
	const auto start = Clock::now ();
	detector.update ();
	worst = std::max (worst, Clock::now () - start);
	std::this_thread::sleep_for (5ms);
    }

    finished = true;
    release.join ();

    CHECK (worst < 100ms);
    CHECK_FALSE (detector.anythingPlaying ());

    server ().thaw ();
    REQUIRE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }, 10s));
}

TEST_CASE ("automute without a reachable sound server does not hang and reports silence") {
    REQUIRE_PRIVATE_SERVER ();

    EnvOverride missing ("PULSE_SERVER", server ().missingSocket ());

    auto context = makeContext ();
    context.settings.audio.automute = true;
    FakeFullScreen fullscreen (context);
    std::unique_ptr<PulseAudioPlayingDetector> detector;

    // a constructor that loops forever is the failure, so it runs on a thread that is left behind when it does
    auto built = std::make_shared<std::promise<void>> ();
    auto result = built->get_future ();

    std::thread ([&, built] {
	detector = std::make_unique<PulseAudioPlayingDetector> (context, fullscreen);
	built->set_value ();
    }).detach ();

    if (result.wait_for (5s) != std::future_status::ready) {
	std::_Exit (1);
    }

    for (int i = 0; i < 20; i++) {
	detector->update ();
    }

    CHECK_FALSE (detector->anythingPlaying ());
}

TEST_CASE ("automute off reports silence and a fullscreen window counts as playing") {
    REQUIRE_PRIVATE_SERVER ();

    SECTION ("automute off") {
	server ().startPlayer ();

	auto context = makeContext ();
	context.settings.audio.automute = false;
	FakeFullScreen fullscreen (context);
	PulseAudioPlayingDetector detector (context, fullscreen);

	REQUIRE_FALSE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }, 700ms));
    }

    SECTION ("fullscreen window") {
	auto context = makeContext ();
	context.settings.audio.automute = true;
	FakeFullScreen fullscreen (context);
	fullscreen.fullscreen = true;
	PulseAudioPlayingDetector detector (context, fullscreen);

	REQUIRE (pumpUntil (detector, [&] { return detector.anythingPlaying (); }));

	fullscreen.fullscreen = false;
	REQUIRE (pumpUntil (detector, [&] { return !detector.anythingPlaying (); }));
    }
}
