#pragma once

#include <string>

#include "WallpaperEngine/Assets/AssetLocator.h"

#include "include/cef_resource_handler.h"
#include "include/wrapper/cef_helpers.h"

namespace WallpaperEngine::Data::Model {
struct Project;
}

namespace WallpaperEngine::WebBrowser::CEF {

using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Data::Model;

/**
 * Serves one wallpaper's files under WPENGINE_SCHEME, bound to that wallpaper's Project by
 * WPSchemeHandlerFactory::Create once it's resolved the request's host to a workshop id.
 */
class WPSchemeHandler : public CefResourceHandler {
public:
    explicit WPSchemeHandler (const Project& project);

    bool Open (CefRefPtr<CefRequest> request, bool& handle_request, CefRefPtr<CefCallback> callback) override;

    void
    GetResponseHeaders (CefRefPtr<CefResponse> response, int64_t& response_length, CefString& redirectUrl) override;

    void Cancel () override;

    bool
    Read (void* data_out, int bytes_to_read, int& bytes_read, CefRefPtr<CefResourceReadCallback> callback) override;

private:
    const Project& m_project;

    const AssetLocator& m_assetLoader;
    ReadStreamSharedPtr m_contents = nullptr;
    std::string m_mimeType;

    // Only set for local files (see LOCAL_FILE_PREFIX), which media elements need Range support for to seek at all
    bool m_isLocalFile = false;
    bool m_partial = false;
    int64_t m_totalSize = 0;
    int64_t m_rangeStart = 0;
    // bytes still to deliver, -1 for "until the stream ends"
    int64_t m_remaining = -1;

    IMPLEMENT_REFCOUNTING (WPSchemeHandler);
    DISALLOW_COPY_AND_ASSIGN (WPSchemeHandler);
};
};