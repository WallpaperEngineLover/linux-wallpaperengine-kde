#pragma once

namespace WallpaperEngine::Render {
class RenderContext;

namespace Helpers {
    /**
     * Small helper class that provides access to the CRenderContext
     * in use currently
     */
    class ContextAware {
    public:
	virtual ~ContextAware () = default;
	ContextAware (const ContextAware& from);
	explicit ContextAware (const ContextAware* from);
	explicit ContextAware (RenderContext& context);

	[[nodiscard]] RenderContext& getContext () const;

    private:
	RenderContext& m_context;
    };
} // namespace Helpers
} // namespace WallpaperEngine::Render