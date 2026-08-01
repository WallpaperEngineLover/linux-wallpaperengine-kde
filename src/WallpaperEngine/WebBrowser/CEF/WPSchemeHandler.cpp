#include "WPSchemeHandler.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include <iostream>

#include "MimeTypes.h"
#include "include/cef_parser.h"

#include "WallpaperEngine/Data/Model/Project.h"

using namespace WallpaperEngine::WebBrowser::CEF;

WPSchemeHandler::WPSchemeHandler (const Project& project) :
    m_project (project), m_assetLoader (*this->m_project.assetLocator) { }

bool WPSchemeHandler::Open (CefRefPtr<CefRequest> request, bool& handle_request, CefRefPtr<CefCallback> callback) {
    DCHECK (!CefCurrentlyOn (TID_UI) && !CefCurrentlyOn (TID_IO));

    // url contains the full path, we need to get rid of the protocol
    // otherwise files won't be found
    CefURLParts parts;

    // url parsing is a must
    if (!CefParseURL (request->GetURL (), parts)) {
	return false;
    }

    std::cout << "Processing request for path " << request->GetURL ().c_str () << std::endl;

    // CefParseURL hands back the path exactly as it appeared in the URL, still percent-encoded -
    // filenames with spaces or other escaped characters (e.g. "Corin%20[M3].png") would otherwise
    // never match the real file ("Corin [M3].png") on disk.
    const std::string path = CefURIDecode (
	CefString (&parts.path), false,
	static_cast<cef_uri_unescape_rule_t> (UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS)
    );

    // path is "/<file>", relative to this wallpaper's own origin (wp://w<id>/...) - the host
    // already picked this handler's m_project, so there's no workshop id prefix to strip here.
    const std::string file = path.size () > 1 ? path.substr (1) : path;

    try {
	// try to read the file on the current container, if the file doesn't exists
	// an exception will be thrown
	if (const char* mime = MimeTypes::getType (file.c_str ()); !mime) {
	    this->m_mimeType = "application/octet+stream";
	} else {
	    this->m_mimeType = mime;
	}

	this->m_contents = this->m_assetLoader.read (file);
	callback->Continue ();
    } catch (AssetLoadException& e) {
	std::cout << "Cannot read file " << file << ": " << e.what () << std::endl;
    }

    handle_request = true;

    return true;
}

void WPSchemeHandler::GetResponseHeaders (
    CefRefPtr<CefResponse> response, int64_t& response_length, CefString& redirectUrl
) {
    CEF_REQUIRE_IO_THREAD ();

    if (!this->m_contents) {
	response->SetError (ERR_FILE_NOT_FOUND);
	response->SetStatus (404);
	response_length = 0;
	return;
    }

    response->SetMimeType (this->m_mimeType);
    response->SetStatus (200);

    // signals an unknown-length file
    response_length = -1;
}

void WPSchemeHandler::Cancel () { CEF_REQUIRE_IO_THREAD (); }

bool WPSchemeHandler::Read (
    void* data_out, int bytes_to_read, int& bytes_read, CefRefPtr<CefResourceReadCallback> callback
) {
    DCHECK (!CefCurrentlyOn (TID_UI) && !CefCurrentlyOn (TID_IO));

    bytes_read = 0;

    if (this->m_contents->eof ()) {
	return false;
    }

    try {
	this->m_contents->read (static_cast<std::istream::char_type*> (data_out), bytes_to_read);
    } catch (std::ios::failure&) {
	bytes_read = -1;
	return false;
    }

    bytes_read = this->m_contents->gcount ();

    return true;
}