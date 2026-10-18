#include "LimitedAllocator.h"

#include <atomic>

namespace WallpaperEngine::Scripting {
namespace {
    class LimitedAllocator final : public v8::ArrayBuffer::Allocator {
    public:
	explicit LimitedAllocator (size_t limit) :
	    m_backing (v8::ArrayBuffer::Allocator::NewDefaultAllocator ()), m_limit (limit) { }

	void* Allocate (size_t length) override {
	    return this->take (length, [&] { return this->m_backing->Allocate (length); });
	}

	void* AllocateUninitialized (size_t length) override {
	    return this->take (length, [&] { return this->m_backing->AllocateUninitialized (length); });
	}

	void Free (void* data, size_t length) override {
	    this->m_backing->Free (data, length);
	    this->m_used -= length;
	}

    private:
	template <typename Allocate> void* take (size_t length, Allocate allocate) {
	    if (this->m_used.fetch_add (length) + length > this->m_limit && this->m_limit > 0) {
		this->m_used -= length;
		return nullptr;
	    }

	    void* data = allocate ();

	    // the system refused it: what was counted for it goes back
	    if (data == nullptr) {
		this->m_used -= length;
	    }

	    return data;
	}

	std::unique_ptr<v8::ArrayBuffer::Allocator> m_backing;
	size_t m_limit;
	std::atomic<size_t> m_used = 0;
    };
}

std::unique_ptr<v8::ArrayBuffer::Allocator> makeLimitedAllocator (size_t limit) {
    return std::make_unique<LimitedAllocator> (limit);
}
}
