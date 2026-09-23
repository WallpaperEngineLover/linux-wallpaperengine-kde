#include "WPSchemeHandler.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "MimeTypes.h"
#include "include/cef_parser.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include <ranges>

using namespace WallpaperEngine::WebBrowser::CEF;

namespace {
// Web wallpapers build "file:///" + <file property> URLs, which a page served from a custom
// scheme can't load. The scripts injected by SubprocessApp rewrite them to this prefix instead.
constexpr auto LOCAL_FILE_PREFIX = "__lwe_file__/";

// A file the user picked through one of the wallpaper's own file properties, or anything inside a directory property
bool isPropertyFile (const Project& project, const std::filesystem::path& path) {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical (path, error);

    if (error) {
	return false;
    }

    for (const auto& property : project.properties | std::views::values) {
	const auto* file = dynamic_cast<const PropertyFile*> (property.get ());

	if (file == nullptr || file->getString ().empty ()) {
	    continue;
	}

	if (!file->isDirectory ()) {
	    if (std::filesystem::path (file->getString ()) == path) {
		return true;
	    }

	    continue;
	}

	const auto directory = std::filesystem::weakly_canonical (file->getString (), error);

	if (error) {
	    continue;
	}

	const auto relative = resolved.lexically_relative (directory);

	if (!relative.empty () && *relative.begin () != "..") {
	    return true;
	}
    }

    return false;
}
} // namespace

WPSchemeHandler::WPSchemeHandler (const Project& project) :
    m_project (project), m_assetLoader (*this->m_project.assetLocator) { }

bool WPSchemeHandler::Open (CefRefPtr<CefRequest> request, bool& handle_request, CefRefPtr<CefCallback> callback) {
    DCHECK (!CefCurrentlyOn (TID_UI) && !CefCurrentlyOn (TID_IO));

    CefURLParts parts;

    if (!CefParseURL (request->GetURL (), parts)) {
	return false;
    }

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
	if (const char* mime = MimeTypes::getType (file.c_str ()); !mime) {
	    this->m_mimeType = "application/octet+stream";
	} else {
	    this->m_mimeType = mime;
	}

	if (file.starts_with (LOCAL_FILE_PREFIX)) {
	    // only files the user picked through (or that live in a folder picked through) this wallpaper's own properties
	    const std::filesystem::path local = "/" + file.substr (std::char_traits<char>::length (LOCAL_FILE_PREFIX));

	    if (!isPropertyFile (this->m_project, local) || !std::filesystem::is_regular_file (local)) {
		throw AssetLoadException (
		    "Not a file or directory property of this wallpaper", local, std::make_error_code (std::errc::permission_denied)
		);
	    }

	    std::cout << "--web-host: serving " << local.string () << " to the page" << std::endl;
	    this->m_contents = std::make_shared<std::ifstream> (local, std::ios::binary);
	    this->m_isLocalFile = true;
	    this->m_totalSize = static_cast<int64_t> (std::filesystem::file_size (local));
	    this->m_remaining = this->m_totalSize;

	    // "bytes=<start>-[<end>]", the only form media elements send
	    if (const std::string range = request->GetHeaderByName ("Range").ToString (); range.starts_with ("bytes=")) {
		const auto dash = range.find ('-');
		const int64_t start = std::atoll (range.c_str () + 6);
		const int64_t last = dash != std::string::npos && dash + 1 < range.size ()
		    ? std::atoll (range.c_str () + dash + 1)
		    : this->m_totalSize - 1;

		if (start >= 0 && start < this->m_totalSize) {
		    this->m_partial = true;
		    this->m_rangeStart = start;
		    this->m_remaining = std::min (last, this->m_totalSize - 1) - start + 1;
		    this->m_contents->seekg (start);
		}
	    }
	} else {
	    this->m_contents = this->m_assetLoader.read (file);
	}

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

    if (this->m_isLocalFile) {
	response->SetHeaderByName ("Accept-Ranges", "bytes", true);

	if (this->m_partial) {
	    response->SetStatus (206);
	    response->SetHeaderByName (
		"Content-Range",
		"bytes " + std::to_string (this->m_rangeStart) + "-"
		    + std::to_string (this->m_rangeStart + this->m_remaining - 1) + "/" + std::to_string (this->m_totalSize),
		true
	    );
	} else {
	    response->SetStatus (200);
	}

	response_length = this->m_remaining;
	return;
    }

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

    if (this->m_contents->eof () || this->m_remaining == 0) {
	return false;
    }

    if (this->m_remaining > 0) {
	bytes_to_read = static_cast<int> (std::min<int64_t> (bytes_to_read, this->m_remaining));
    }

    try {
	this->m_contents->read (static_cast<std::istream::char_type*> (data_out), bytes_to_read);
    } catch (std::ios::failure&) {
	bytes_read = -1;
	return false;
    }

    bytes_read = this->m_contents->gcount ();

    if (this->m_remaining > 0) {
	this->m_remaining -= bytes_read;
    }

    return true;
}