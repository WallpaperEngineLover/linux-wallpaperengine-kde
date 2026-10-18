#pragma once

#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <vector>

namespace WallpaperEngine::Logging {
class Log {
public:
    void addOutput (std::ostream* stream);
    void addError (std::ostream* stream);

    template <typename... Data> void out (Data... data) {
	std::string str = this->buildBuffer (data...);
	const std::lock_guard lock (this->mMutex);

	for (const auto cur : this->mOutputs) {
	    *cur << str << '\n' << std::flush;
	}
    }

    template <typename... Data> void debug (Data... data) {
#if (!NDEBUG) && (!ERRORONLY)
	std::string str = this->buildBuffer (data...);
	const std::lock_guard lock (this->mMutex);

	for (const auto cur : this->mOutputs) {
	    *cur << str << '\n';
	}
#endif
    }

    template <typename... Data> void debugerror (Data... data) {
#if (!NDEBUG) && (ERRORONLY)
	std::string str = this->buildBuffer (data...);
	const std::lock_guard lock (this->mMutex);

	for (const auto cur : this->mOutputs) {
	    *cur << str << '\n';
	}
#endif
    }

    template <typename... Data> void error (Data... data) {
	std::string str = this->buildBuffer (data...);
	const std::lock_guard lock (this->mMutex);

	for (const auto cur : this->mErrors) {
	    *cur << str << '\n' << std::flush;
	}
    }

    template <class EX, typename... Data> [[noreturn]] void exception (Data... data) {
	std::string str = this->buildBuffer (data...);

	{
	    const std::lock_guard lock (this->mMutex);

	    for (const auto cur : this->mErrors) {
		*cur << str << '\n';
	    }
	}

	throw EX (str);
    }

    template <typename... Data> [[noreturn]] void exception (Data... data) {
	this->exception<std::runtime_error> (data...);
    }

    static Log& get ();

private:
    Log ();

    template <typename... Data> std::string buildBuffer (Data... data) {
	std::stringbuf buffer;
	std::ostream bufferStream (&buffer);

	((bufferStream << std::forward<Data> (data)), ...);

	return buffer.str ();
    }

    std::mutex mMutex;
    std::vector<std::ostream*> mOutputs = {};
    std::vector<std::ostream*> mErrors = {};
    static std::unique_ptr<Log> sInstance;
};
}

#define sLog (WallpaperEngine::Logging::Log::get ())