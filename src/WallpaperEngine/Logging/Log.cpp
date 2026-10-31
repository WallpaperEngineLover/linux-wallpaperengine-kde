#include "Log.h"

#include <cassert>
#include <memory>

using namespace WallpaperEngine::Logging;

Log::Log () { assert (this->sInstance == nullptr); }

Log& Log::get () {
    static Log* const instance = [] {
	sInstance = std::unique_ptr<Log> (new Log ());
	return sInstance.get ();
    }();

    return *instance;
}

void Log::addOutput (std::ostream* stream) {
    const Debugging::LockGuard lock (this->mMutex);
    this->mOutputs.push_back (stream);
}

void Log::addError (std::ostream* stream) {
    const Debugging::LockGuard lock (this->mMutex);
    this->mErrors.push_back (stream);
}

std::unique_ptr<Log> Log::sInstance = nullptr;