#include "X11DesktopIcons.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

using namespace WallpaperEngine::Render::Drivers::Output;

namespace {
// black: the antialiased edge of an icon or a label blends with it, which reads as a thin shadow on the wallpaper where
// any other key color leaves a fringe of its own
constexpr int KEY_TOLERANCE = 2;
constexpr int OPAQUE_DISTANCE = 48;
constexpr const char* XFCE_CHANNEL = "xfce4-desktop";

bool runCapture (const std::vector<std::string>& argv, std::string* output = nullptr) {
    int fds[2];

    if (pipe (fds) != 0) {
	return false;
    }

    // everything that allocates happens before the fork, the engine has threads
    std::vector<char*> args;
    args.reserve (argv.size () + 1);

    for (const auto& arg : argv) {
	args.push_back (const_cast<char*> (arg.c_str ()));
    }

    args.push_back (nullptr);
    const pid_t pid = fork ();

    if (pid < 0) {
	close (fds[0]);
	close (fds[1]);
	return false;
    }

    if (pid == 0) {
	dup2 (fds[1], STDOUT_FILENO);
	const int null = open ("/dev/null", O_RDWR);

	if (null >= 0) {
	    dup2 (null, STDIN_FILENO);
	    dup2 (null, STDERR_FILENO);
	}

	close (fds[0]);
	close (fds[1]);
	execvp (args[0], args.data ());
	_exit (127);
    }

    close (fds[1]);
    std::string text;
    char buffer[4096];
    ssize_t count;

    while ((count = read (fds[0], buffer, sizeof (buffer))) > 0) {
	text.append (buffer, static_cast<size_t> (count));
    }

    close (fds[0]);
    int status = 0;
    waitpid (pid, &status, 0);

    if (output != nullptr) {
	*output = std::move (text);
    }

    return WIFEXITED (status) && WEXITSTATUS (status) == 0;
}

std::string quote (const std::string& text) {
    std::string out = "'";

    for (const char c : text) {
	if (c == '\'') {
	    out += "'\\''";
	} else {
	    out += c;
	}
    }

    out += "'";
    return out;
}

std::vector<std::string> lines (const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream (text);
    std::string line;

    while (std::getline (stream, line)) {
	if (!line.empty ()) {
	    out.push_back (line);
	}
    }

    return out;
}

int number (const std::string& text, size_t from) {
    return static_cast<int> (std::strtol (text.c_str () + from, nullptr, 10));
}

std::string restoreFilePath () {
    const char* runtime = std::getenv ("XDG_RUNTIME_DIR");
    std::string path = runtime != nullptr && *runtime != '\0' ? runtime : "/tmp";
    path += "/lwe-xfdesktop-restore." + std::to_string (getuid ()) + ".sh";
    return path;
}

/** a zombie that nobody reaped is dead for our purposes */
bool alive (pid_t pid) {
    if (pid <= 0) {
	return false;
    }

    std::ifstream status ("/proc/" + std::to_string (pid) + "/status");
    std::string line;

    while (std::getline (status, line)) {
	if (line.rfind ("State:", 0) == 0) {
	    return line.find ('Z') == std::string::npos;
	}
    }

    return false;
}

pid_t restoreOwner (const std::string& path) {
    std::ifstream file (path);
    std::string first;

    if (!std::getline (file, first) || first.rfind ("# ", 0) != 0) {
	return 0;
    }

    return static_cast<pid_t> (number (first, 2));
}

/** runs the restore file of one engine, once: whoever renames it first is the one to run it (the engine itself on the
 * way out, the watcher after a crash), and never the file of another engine that started in between */
void runRestore (const std::string& path, pid_t owner) {
    const char* script
	= R"sh(f="$1"; [ "$(sed -n 1p "$f" 2>/dev/null)" = "# $2" ] || exit 0; t="$f.run.$$"; mv "$f" "$t" 2>/dev/null || exit 0; sh "$t"; rm -f "$t")sh";
    runCapture ({ "sh", "-c", script, "sh", path, std::to_string (owner) });
}

/** a wallpaper for a desktop that never picked one: xfdesktop's own default can't be named and xfdesktop 4.20 without a
 * last-image spins at full CPU and leaks memory once its style goes back to an image */
std::string defaultWallpaper () {
    for (const char* name : { "xfce-shapes.svg", "xfce-blue.jpg", "xfce-teal.jpg", "xfce-leaves.svg", "xfce-light.svg",
			      "xfce-x.svg", "xfce-verticals.png" }) {
	const std::string path = std::string ("/usr/share/backgrounds/xfce/") + name;

	if (access (path.c_str (), R_OK) == 0) {
	    return path;
	}
    }

    return {};
}

class XfceBackdrop {
public:
    bool apply (const std::vector<std::string>& outputNames) {
	const std::string path = restoreFilePath ();

	if (const pid_t owner = restoreOwner (path); owner != 0) {
	    if (alive (owner) && owner != getpid ()) {
		sLog.out ("Another engine already keys xfdesktop's background, sharing it");
		return true;
	    }

	    runRestore (path, owner);
	}

	std::string listing;

	if (!runCapture ({ "xfconf-query", "-c", XFCE_CHANNEL, "-l" }, &listing)) {
	    return false;
	}

	std::set<std::string> prefixes;
	std::set<int> screens { 0 };
	std::set<int> workspaces { 0 };

	for (const auto& property : lines (listing)) {
	    const auto workspace = property.find ("/workspace");

	    if (property.rfind ("/backdrop/screen", 0) != 0 || workspace == std::string::npos) {
		continue;
	    }

	    prefixes.insert (property.substr (0, property.find ('/', workspace + 1)));
	    screens.insert (number (property, 16));
	    workspaces.insert (number (property, workspace + 10));
	}

	// xfdesktop names a monitor by its index or by its output, depending on version and settings
	for (const int screen : screens) {
	    for (const int workspace : workspaces) {
		for (size_t i = 0; i < std::max<size_t> (outputNames.size (), 1); i++) {
		    const std::string base = "/backdrop/screen" + std::to_string (screen) + "/monitor";
		    const std::string suffix = "/workspace" + std::to_string (workspace);
		    prefixes.insert (base + std::to_string (i) + suffix);

		    if (i < outputNames.size ()) {
			prefixes.insert (base + outputNames[i] + suffix);
		    }
		}
	    }
	}

	std::ostringstream restore;
	restore << "# " << getpid () << "\n";
	bool changed = false;
	const std::string fallback = defaultWallpaper ();

	for (const auto& prefix : prefixes) {
	    if (!fallback.empty ()
		&& !runCapture ({ "xfconf-query", "-c", XFCE_CHANNEL, "-p", prefix + "/last-image" })) {
		restore << "xfconf-query -c " << XFCE_CHANNEL << " -p " << quote (prefix + "/last-image")
			<< " -n -t string -s " << quote (fallback) << "\n";
	    }

	    for (const char* name : { "image-style", "color-style", "rgba1" }) {
		const std::string property = prefix + "/" + name;
		std::string old;
		const bool existed = runCapture ({ "xfconf-query", "-c", XFCE_CHANNEL, "-p", property }, &old);
		std::vector<std::string> command { "xfconf-query", "-c", XFCE_CHANNEL, "-p", property };
		std::string restoreArgs;

		if (std::strcmp (name, "rgba1") == 0) {
		    auto values = lines (old);

		    if (existed && values.size () >= 4) {
			values.erase (values.begin (), values.end () - 4);
			restoreArgs = "-t double -t double -t double -t double";

			for (const auto& value : values) {
			    restoreArgs += " -s " + quote (value);
			}
		    }

		    command.insert (
			command.end (),
			{ "-n", "-t", "double", "-t", "double", "-t", "double", "-t", "double", "-s", "0.0", "-s",
			  "0.0", "-s", "0.0", "-s", "1.0" }
		    );
		} else {
		    const auto values = lines (old);

		    if (existed && !values.empty ()) {
			restoreArgs = "-s " + quote (values.back ());
		    }

		    command.insert (command.end (), { "-n", "-t", "int", "-s", "0" });
		}

		if (!runCapture (command)) {
		    continue;
		}

		changed = true;

		if (existed && !restoreArgs.empty ()) {
		    restore << "xfconf-query -c " << XFCE_CHANNEL << " -p " << quote (property) << " " << restoreArgs
			    << "\n";
		    continue;
		}

		// a property that was not there gets xfdesktop's own default and stays: xfdesktop 4.20 spins at full
		// CPU and leaks memory after a property it reads was removed, and removing would not make it paint
		// again anyway
		if (std::strcmp (name, "image-style") == 0) {
		    restore << "xfconf-query -c " << XFCE_CHANNEL << " -p " << quote (property) << " -n -t int -s 5\n";
		} else if (std::strcmp (name, "color-style") == 0) {
		    restore << "xfconf-query -c " << XFCE_CHANNEL << " -p " << quote (property) << " -n -t int -s 0\n";
		}
	    }
	}

	if (!changed) {
	    return false;
	}

	{
	    std::ofstream file (path, std::ios::trunc);
	    file << restore.str ();
	}

	chmod (path.c_str (), 0600);
	this->m_path = path;
	this->m_owner = getpid ();
	this->watch ();
	return true;
    }

    void restore () {
	if (this->m_path.empty () || this->m_owner != getpid ()) {
	    return;
	}

	runRestore (this->m_path, this->m_owner);
	this->m_path.clear ();
	this->m_owner = 0;
    }

private:
    /** a SIGKILLed engine would leave the desktop with the key color, so a shell waits for it and puts the settings
     * back */
    void watch () const {
	const std::string status = "/proc/" + std::to_string (this->m_owner) + "/status";
	std::string script = "while [ -r " + status + " ] && ! grep -q '^State:.*Z' " + status + "; do sleep 1; done; ";
	script
	    += R"sh(f="$1"; [ "$(sed -n 1p "$f" 2>/dev/null)" = "# $2" ] || exit 0; t="$f.run.$$"; mv "$f" "$t" 2>/dev/null || exit 0; sh "$t"; rm -f "$t")sh";
	const std::string path = this->m_path;
	const std::string owner = std::to_string (this->m_owner);
	const pid_t pid = fork ();

	if (pid != 0) {
	    return;
	}

	setsid ();
	const int null = open ("/dev/null", O_RDWR);

	if (null >= 0) {
	    dup2 (null, STDIN_FILENO);
	    dup2 (null, STDOUT_FILENO);
	    dup2 (null, STDERR_FILENO);
	}

	execl (
	    "/bin/sh", "sh", "-c", script.c_str (), "sh", path.c_str (), owner.c_str (), static_cast<char*> (nullptr)
	);
	_exit (127);
    }

    std::string m_path;
    pid_t m_owner = 0;
};

XfceBackdrop& backdrop () {
    static XfceBackdrop instance;
    return instance;
}

bool classContains (Display* display, Window window, const char* needle) {
    XClassHint hint {};

    if (!XGetClassHint (display, window, &hint)) {
	return false;
    }

    std::string text = std::string (hint.res_class != nullptr ? hint.res_class : "") + " "
	+ (hint.res_name != nullptr ? hint.res_name : "");

    if (hint.res_class != nullptr) {
	XFree (hint.res_class);
    }

    if (hint.res_name != nullptr) {
	XFree (hint.res_name);
    }

    std::transform (text.begin (), text.end (), text.begin (), [] (unsigned char c) { return std::tolower (c); });
    return text.find (needle) != std::string::npos;
}
}

X11DesktopIcons::X11DesktopIcons (Display* display, Window root, int fullWidth, int fullHeight) :
    m_display (display), m_root (root), m_fullWidth (fullWidth), m_fullHeight (fullHeight) { }

X11DesktopIcons::~X11DesktopIcons () {
    for (auto& tracked : this->m_tracked) {
	this->untrack (tracked);
    }

    if (this->m_active) {
	backdrop ().restore ();
    }
}

bool X11DesktopIcons::supports (Display* display, Window window) {
    return classContains (display, window, "xfdesktop");
}

bool X11DesktopIcons::start (const std::vector<Window>& windows, const std::vector<std::string>& outputNames) {
    int compositeBase, compositeError, damageError;

    if (!XCompositeQueryExtension (this->m_display, &compositeBase, &compositeError)
	|| !XDamageQueryExtension (this->m_display, &this->m_damageEvent, &damageError)) {
	sLog.out ("Composite or Damage is missing, the desktop's icons stay under the wallpaper");
	return false;
    }

    int major = 0, minor = 0;
    XCompositeQueryVersion (this->m_display, &major, &minor);

    if (major == 0 && minor < 2) {
	return false;
    }

    // following first: xfdesktop paints the key color as soon as the setting changes and that damage has to be seen
    this->follow (windows);

    if (!backdrop ().apply (outputNames)) {
	sLog.out ("xfdesktop's background could not be set to the key color, its icons stay under the wallpaper");
	this->follow ({});
	return false;
    }

    this->m_active = true;
    sLog.out ("xfdesktop's icons are drawn over the wallpaper, its background is black while the engine runs");
    return true;
}

void X11DesktopIcons::follow (const std::vector<Window>& windows) {
    std::erase_if (this->m_tracked, [&] (Tracked& tracked) {
	if (std::find (windows.begin (), windows.end (), tracked.window) != windows.end ()) {
	    return false;
	}

	this->untrack (tracked);
	return true;
    });

    for (const Window window : windows) {
	const bool known = std::ranges::any_of (this->m_tracked, [&] (const Tracked& t) { return t.window == window; });

	if (!known) {
	    this->track (window);
	}
    }

    for (auto& tracked : this->m_tracked) {
	XWindowAttributes attributes;

	if (!XGetWindowAttributes (this->m_display, tracked.window, &attributes)) {
	    continue;
	}

	int x = 0, y = 0;
	Window child;
	XTranslateCoordinates (this->m_display, tracked.window, this->m_root, 0, 0, &x, &y, &child);

	if (x != tracked.x || y != tracked.y || attributes.width != tracked.width
	    || attributes.height != tracked.height) {
	    tracked.x = x;
	    tracked.y = y;
	    tracked.width = attributes.width;
	    tracked.height = attributes.height;
	    tracked.dirty = true;
	}
    }
}

void X11DesktopIcons::track (Window window) {
    Tracked tracked;
    tracked.window = window;
    XCompositeRedirectWindow (this->m_display, window, CompositeRedirectAutomatic);
    tracked.damage = XDamageCreate (this->m_display, window, XDamageReportNonEmpty);
    this->m_tracked.push_back (tracked);
}

void X11DesktopIcons::untrack (Tracked& tracked) {
    if (tracked.damage != 0) {
	XDamageDestroy (this->m_display, tracked.damage);
	tracked.damage = 0;
    }

    XCompositeUnredirectWindow (this->m_display, tracked.window, CompositeRedirectAutomatic);
}

bool X11DesktopIcons::handleEvent (const XEvent& event) {
    if (event.type != this->m_damageEvent + XDamageNotify) {
	return false;
    }

    const auto& damage = reinterpret_cast<const XDamageNotifyEvent&> (event);

    for (auto& tracked : this->m_tracked) {
	if (tracked.damage == damage.damage) {
	    tracked.dirty = true;
	    XDamageSubtract (this->m_display, tracked.damage, None, None);
	}
    }

    return true;
}

void X11DesktopIcons::update () {
    const auto now = std::chrono::steady_clock::now ();

    for (auto& tracked : this->m_tracked) {
	if (tracked.dirty && now >= tracked.retryAt) {
	    tracked.dirty = false;
	    this->capture (tracked);
	}
    }
}

void X11DesktopIcons::capture (Tracked& tracked) {
    auto& out = tracked.icons;
    out.clear ();

    if (tracked.width <= 0 || tracked.height <= 0) {
	return;
    }

    const Pixmap pixmap = XCompositeNameWindowPixmap (this->m_display, tracked.window);
    XImage* image = XGetImage (this->m_display, pixmap, 0, 0, tracked.width, tracked.height, AllPlanes, ZPixmap);
    XFreePixmap (this->m_display, pixmap);

    if (image == nullptr) {
	return;
    }

    // a pixmap has no visual and so no color masks: 24 bit depth in 32 bits, B G R X in memory on a little endian
    // server
    if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst) {
	XDestroyImage (image);
	return;
    }

    // the window only counts once it paints the key color: before xfdesktop took the setting up it still shows its own
    // wallpaper, which would otherwise cover the real one
    size_t keyed = 0;
    size_t sampled = 0;

    for (int row = 0; row < tracked.height; row += 4) {
	const auto* line
	    = reinterpret_cast<const uint8_t*> (image->data) + static_cast<size_t> (row) * image->bytes_per_line;

	for (int column = 0; column < tracked.width; column += 4) {
	    const uint8_t* p = line + column * 4;
	    keyed += std::max ({ p[0], p[1], p[2] }) <= KEY_TOLERANCE;
	    sampled++;
	}
    }

    if (keyed * 2 < sampled) {
	XDestroyImage (image);

	if (++tracked.attempts < 40) {
	    tracked.dirty = true;
	    tracked.retryAt = std::chrono::steady_clock::now () + std::chrono::milliseconds (500);
	}

	return;
    }

    tracked.attempts = 0;

    for (int row = 0; row < tracked.height; row++) {
	const int destRow = tracked.y + row;

	if (destRow < 0 || destRow >= this->m_fullHeight) {
	    continue;
	}

	const auto* line
	    = reinterpret_cast<const uint8_t*> (image->data) + static_cast<size_t> (row) * image->bytes_per_line;

	for (int column = 0; column < tracked.width; column++) {
	    const int destColumn = tracked.x + column;

	    if (destColumn < 0 || destColumn >= this->m_fullWidth) {
		continue;
	    }

	    const uint8_t* p = line + column * 4;
	    const int distance = std::max ({ p[0], p[1], p[2] });

	    if (distance <= KEY_TOLERANCE) {
		continue;
	    }

	    const int alpha = distance >= OPAQUE_DISTANCE ? 255 : distance * 255 / OPAQUE_DISTANCE;
	    out.push_back (
		{ static_cast<uint32_t> (destRow) * this->m_fullWidth + destColumn, p[0], p[1], p[2],
		  static_cast<uint8_t> (alpha) }
	    );
	}
    }

    XDestroyImage (image);
}

void X11DesktopIcons::composite (char* frame, int width, int height) const {
    if (width != this->m_fullWidth || height != this->m_fullHeight) {
	return;
    }

    auto* pixels = reinterpret_cast<uint8_t*> (frame);

    for (const auto& tracked : this->m_tracked) {
	for (const auto& icon : tracked.icons) {
	    uint8_t* d = pixels + static_cast<size_t> (icon.index) * 4;
	    const int a = icon.a;
	    d[0] = static_cast<uint8_t> ((d[0] * (255 - a) + icon.b * a) / 255);
	    d[1] = static_cast<uint8_t> ((d[1] * (255 - a) + icon.g * a) / 255);
	    d[2] = static_cast<uint8_t> ((d[2] * (255 - a) + icon.r * a) / 255);
	}
    }
}
