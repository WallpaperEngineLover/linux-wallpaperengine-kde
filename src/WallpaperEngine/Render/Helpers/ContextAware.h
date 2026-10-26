#pragma once

namespace WallpaperEngine::Render {
class RenderContext;

namespace Helpers {
    class ContextAware {
    public:
	virtual ~ContextAware () = default;
	ContextAware (const ContextAware& from);
	ContextAware& operator= (const ContextAware& from) = delete;
	explicit ContextAware (const ContextAware* from);
	explicit ContextAware (RenderContext& context);

	[[nodiscard]] RenderContext& getContext () const;

    private:
	RenderContext& m_context;
    };
}
}