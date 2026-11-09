#pragma once

#include <cstdlib>
#include <dlfcn.h>

namespace WallpaperEngine::Desktop {
/** libX11 and one of its extension libraries through dlopen, none of them is a build dependency */
class DlopenX11 {
public:
    DlopenX11 () = default;

    ~DlopenX11 () {
	if (this->m_display != nullptr) {
	    this->m_closeDisplay (this->m_display);
	}

	this->unload ();
    }

    DlopenX11 (const DlopenX11&) = delete;
    DlopenX11& operator= (const DlopenX11&) = delete;

    /** Loads both libraries, false when any of it is missing */
    bool load (const char* extensionLibrary) {
	if (this->m_x11 != nullptr && this->m_extension != nullptr) {
	    return true;
	}

	this->unload ();

	if (std::getenv ("DISPLAY") == nullptr) {
	    return false;
	}

	this->m_x11 = dlopen ("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
	this->m_extension = dlopen (extensionLibrary, RTLD_NOW | RTLD_LOCAL);

	if (this->m_x11 == nullptr || this->m_extension == nullptr) {
	    this->unload ();
	    return false;
	}

	this->m_openDisplay = this->x11Symbol<void* (*)(const char*)> ("XOpenDisplay");
	this->m_closeDisplay = this->x11Symbol<int (*) (void*)> ("XCloseDisplay");

	if (this->m_openDisplay == nullptr || this->m_closeDisplay == nullptr) {
	    this->unload ();
	    return false;
	}

	return true;
    }

    /** Opens the display, once the caller found every symbol it needs */
    bool connect () {
	if (this->m_display == nullptr && this->m_openDisplay != nullptr) {
	    this->m_display = this->m_openDisplay (nullptr);
	}

	return this->m_display != nullptr;
    }

    [[nodiscard]] void* display () const { return this->m_display; }

    template <typename Function> Function x11Symbol (const char* name) const {
	return reinterpret_cast<Function> (dlsym (this->m_x11, name));
    }

    template <typename Function> Function extensionSymbol (const char* name) const {
	return reinterpret_cast<Function> (dlsym (this->m_extension, name));
    }

private:
    void unload () {
	if (this->m_extension != nullptr) {
	    dlclose (this->m_extension);
	    this->m_extension = nullptr;
	}

	if (this->m_x11 != nullptr) {
	    dlclose (this->m_x11);
	    this->m_x11 = nullptr;
	}

	this->m_openDisplay = nullptr;
	this->m_closeDisplay = nullptr;
    }

    void* m_x11 = nullptr;
    void* m_extension = nullptr;
    void* m_display = nullptr;
    void* (*m_openDisplay) (const char*) = nullptr;
    int (*m_closeDisplay) (void*) = nullptr;
};
}
