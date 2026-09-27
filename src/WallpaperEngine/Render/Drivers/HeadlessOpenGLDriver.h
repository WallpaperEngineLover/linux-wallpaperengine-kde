#pragma once

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Input/MouseInput.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

#include <EGL/egl.h>
#include <GL/glew.h>
#include <chrono>

namespace WallpaperEngine::Render::Drivers {
using namespace WallpaperEngine::Application;

/**
 * Renders into an offscreen EGL pbuffer on a GPU render node, no window system involved. Picked with
 * XDG_SESSION_TYPE=headless together with --window, meant for screenshots and regression runs on
 * machines without a display (Xvfb only offers software rendering). LWE_HEADLESS_DEVICE=/dev/dri/renderDN
 * chooses the GPU, otherwise the first one EGL lists is used.
 */
class HeadlessOpenGLDriver final : public VideoDriver {
public:
    explicit HeadlessOpenGLDriver (ApplicationContext& context, WallpaperApplication& app);
    ~HeadlessOpenGLDriver () override;

    [[nodiscard]] Output::Output& getOutput () override;
    [[nodiscard]] float getRenderTime () const override;
    bool closeRequested () override;
    void resizeWindow (glm::ivec2 size) override;
    void resizeWindow (glm::ivec4 sizeandpos) override;
    void showWindow () override;
    void hideWindow () override;
    [[nodiscard]] glm::ivec2 getFramebufferSize () const override;
    [[nodiscard]] uint32_t getFrameCounter () const override;
    void dispatchEventQueue () override;
    [[nodiscard]] void* getProcAddress (const char* name) const override;

private:
    class FixedMouse final : public Input::MouseInput {
    public:
	explicit FixedMouse (const HeadlessOpenGLDriver& driver);

	void update () override { }
	// the middle of the output (where the pointer of a fresh Xvfb display sits too), or LWE_HEADLESS_CURSOR=x,y
	// as fractions of the output
	[[nodiscard]] glm::dvec2 position () const override { return glm::dvec2 (m_driver.m_size) * m_fraction; }
	[[nodiscard]] Input::MouseClickStatus leftClick () const override { return Input::Released; }
	[[nodiscard]] Input::MouseClickStatus rightClick () const override { return Input::Released; }

    private:
	const HeadlessOpenGLDriver& m_driver;
	glm::dvec2 m_fraction = glm::dvec2 (0.5);
    };

    void createSurface ();

    FixedMouse m_mouseInput;
    Output::Output* m_output = nullptr;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
    GLsync m_lastFrame = nullptr;
    glm::ivec2 m_size = { 640, 480 };
    std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now ();
    uint32_t m_frameCounter = 0;
};
} // namespace WallpaperEngine::Render::Drivers
