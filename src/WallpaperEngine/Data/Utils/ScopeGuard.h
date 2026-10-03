#pragma once
#include <utility>

namespace WallpaperEngine::Data::Utils {
template <typename F> struct ScopeGuard {
    ScopeGuard (const ScopeGuard& other) = delete;
    ScopeGuard& operator= (const ScopeGuard& other) = delete;

    explicit ScopeGuard (F&& f) : func (std::forward<F> (f)), owner (true) { }
    explicit ScopeGuard (ScopeGuard&& other) noexcept : func (std::move (other.func)), owner (other.owner) {
	other.owner = false;
    }
    ~ScopeGuard () { this->execute (); }

    void cancel () { this->owner = false; }

protected:
    void execute () {
	if (this->owner) {
	    this->func ();
	}

	this->owner = false;
    }

    F func;
    bool owner;
};
};