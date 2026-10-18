#pragma once

#include <cstddef>
#include <memory>

#include <v8-array-buffer.h>

namespace WallpaperEngine::Scripting {
/**
 * The isolate's ArrayBuffer allocator with a cap on what scripts can hold. ArrayBuffer memory lives outside V8's heap
 * and its limit, a script keeping Float64Arrays around would grow the process until the machine runs out. Past the
 * limit the allocation fails and the script gets V8's RangeError; a limit of 0 means none. Nothing native may allocate
 * through it, V8 aborts the process when ArrayBuffer::New fails (EngineObject keeps its own memory).
 *
 * A function and not the class: libnode is built without RTTI and does not export typeinfo for V8's classes, and
 * depending on the V8 version a class derived from ArrayBuffer::Allocator needs it ("undefined reference to typeinfo
 * for v8::ArrayBuffer::Allocator" at link time). LimitedAllocator.cpp is the one file that derives from it and is
 * compiled without RTTI, like V8 itself.
 */
std::unique_ptr<v8::ArrayBuffer::Allocator> makeLimitedAllocator (size_t limit);
} // namespace WallpaperEngine::Scripting
