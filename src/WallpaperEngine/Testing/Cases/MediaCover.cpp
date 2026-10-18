#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "WallpaperEngine/Media/CoverDownloader.h"
#include "WallpaperEngine/Media/CoverImage.h"

using namespace WallpaperEngine::Media;

namespace {
// 4x2, red with the bottom right pixel blue
const std::vector<uint8_t> PNG_COVER = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x7f, 0xa8, 0x7d,
    0x63, 0x00, 0x00, 0x00, 0x17, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xfc, 0xcf, 0xc0, 0xf0,
    0x9f, 0x01, 0x09, 0x30, 0x21, 0x73, 0x18, 0x19, 0xfe, 0x33, 0x00, 0x00, 0x41, 0x29, 0x03, 0x02,
    0xb6, 0x2f, 0x64, 0xee, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

const std::vector<uint8_t> WEBP_COVER = {
    0x52, 0x49, 0x46, 0x46, 0x20, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50,
    0x38, 0x4c, 0x13, 0x00, 0x00, 0x00, 0x2f, 0x03, 0x40, 0x00, 0x00, 0x0f, 0x10, 0xf3,
    0xbf, 0xff, 0xf3, 0x1f, 0x0e, 0x7e, 0xf0, 0x10, 0xd1, 0xff, 0x08, 0x00,
};

void expectRedWithBlueCorner (const std::optional<CoverImage>& image) {
    REQUIRE (image.has_value ());
    REQUIRE (image->width == 4);
    REQUIRE (image->height == 2);
    CHECK (image->rgba[0] == 255);
    CHECK (image->rgba[1] == 0);
    CHECK (image->rgba[2] == 0);
    const size_t corner = (1 * 4 + 3) * 4;
    CHECK (image->rgba[corner] == 0);
    CHECK (image->rgba[corner + 2] == 255);
}

// answers every request on localhost with one canned response chosen by the path
class CoverServer {
public:
    CoverServer () {
	this->m_socket = socket (AF_INET, SOCK_STREAM, 0);

	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
	REQUIRE (bind (this->m_socket, reinterpret_cast<sockaddr*> (&address), sizeof (address)) == 0);
	REQUIRE (listen (this->m_socket, 8) == 0);

	socklen_t length = sizeof (address);
	getsockname (this->m_socket, reinterpret_cast<sockaddr*> (&address), &length);
	this->m_port = ntohs (address.sin_port);

	this->m_thread = std::thread ([this] { this->serve (); });
    }

    ~CoverServer () {
	this->m_stop = true;
	shutdown (this->m_socket, SHUT_RDWR);
	close (this->m_socket);
	this->m_thread.join ();
    }

    [[nodiscard]] std::string url (const std::string& path) const {
	return "http://127.0.0.1:" + std::to_string (this->m_port) + path;
    }

    std::atomic<int> requests = 0;

private:
    void serve () {
	while (!this->m_stop) {
	    const int client = accept (this->m_socket, nullptr, nullptr);

	    if (client < 0) {
		return;
	    }

	    std::string request;
	    char chunk[1024];

	    while (request.find ("\r\n\r\n") == std::string::npos) {
		const auto count = recv (client, chunk, sizeof (chunk), 0);

		if (count <= 0) {
		    break;
		}

		request.append (chunk, static_cast<size_t> (count));
	    }

	    this->requests++;
	    const auto path = request.substr (
		request.find (' ') + 1, request.find (' ', request.find (' ') + 1) - request.find (' ') - 1
	    );
	    std::string header = "HTTP/1.1 200 OK\r\n";
	    std::string body;

	    if (path == "/cover.png") {
		body.assign (PNG_COVER.begin (), PNG_COVER.end ());
	    } else if (path == "/cover.webp") {
		body.assign (WEBP_COVER.begin (), WEBP_COVER.end ());
	    } else if (path == "/redirect") {
		header = "HTTP/1.1 302 Found\r\nLocation: /cover.png\r\n";
	    } else if (path == "/page.html") {
		body = "<html><body>not an image</body></html>";
	    } else {
		header = "HTTP/1.1 404 Not Found\r\n";
		body = "missing";
	    }

	    const std::string response
		= header + "Content-Length: " + std::to_string (body.size ()) + "\r\nConnection: close\r\n\r\n" + body;
	    send (client, response.data (), response.size (), MSG_NOSIGNAL);
	    close (client);
	}
    }

    int m_socket = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_stop = false;
    std::thread m_thread;
};

std::optional<CoverDownloader::Finished> waitFor (CoverDownloader& downloader, const std::string& url) {
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);

    while (std::chrono::steady_clock::now () < deadline) {
	for (auto& finished : downloader.collect ()) {
	    if (finished.url == url) {
		return finished;
	    }
	}

	std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    return std::nullopt;
}
}

TEST_CASE ("Cover URLs are percent decoded and only file:// ones are local paths") {
    CHECK (localCoverPath ("file:///home/user/My%20Music/cover.jpg") == "/home/user/My Music/cover.jpg");
    CHECK (localCoverPath ("file://localhost/tmp/a.png") == "/tmp/a.png");
    CHECK (localCoverPath ("file:///tmp/caf%C3%A9.png") == "/tmp/caf\xC3\xA9.png");
    CHECK (localCoverPath ("file:///tmp/a.png?x=1#y") == "/tmp/a.png");

    CHECK_FALSE (localCoverPath ("https://i.scdn.co/image/abc").has_value ());
    CHECK_FALSE (localCoverPath ("file://relative/path.png").has_value ());
    CHECK_FALSE (localCoverPath ("file:///tmp/a%zz.png").has_value ());
    CHECK_FALSE (localCoverPath ("file:///tmp/a%2").has_value ());
    CHECK_FALSE (localCoverPath ("file:///tmp/a%00b.png").has_value ());
    CHECK_FALSE (localCoverPath ("").has_value ());
}

TEST_CASE ("Covers decode by their bytes, not their extension") {
    expectRedWithBlueCorner (decodeCoverImage (PNG_COVER));
    expectRedWithBlueCorner (decodeCoverImage (WEBP_COVER));

    const auto directory = std::filesystem::temp_directory_path () / "lwe-cover-test";
    std::filesystem::create_directories (directory);
    const auto file = directory / "cover.jpg";

    {
	std::ofstream out (file, std::ios::binary);
	out.write (
	    reinterpret_cast<const char*> (WEBP_COVER.data ()), static_cast<std::streamsize> (WEBP_COVER.size ())
	);
    }

    expectRedWithBlueCorner (loadCoverImage (file));
    std::filesystem::remove_all (directory);

    CHECK_FALSE (loadCoverImage (directory / "missing.png").has_value ());
}

TEST_CASE ("Anything that is not a usable image is refused") {
    CHECK_FALSE (decodeCoverImage ({}).has_value ());

    const std::string html = "<html>nope</html>";
    CHECK_FALSE (
	decodeCoverImage (std::span (reinterpret_cast<const uint8_t*> (html.data ()), html.size ())).has_value ()
    );

    auto truncated = WEBP_COVER;
    truncated.resize (20);
    CHECK_FALSE (decodeCoverImage (truncated).has_value ());
}

TEST_CASE ("Cover art is downloaded in the background and kept in the cache") {
    const auto cache = std::filesystem::temp_directory_path () / "lwe-cover-cache-test";
    std::filesystem::remove_all (cache);
    setenv ("XDG_CACHE_HOME", cache.c_str (), 1);

    CoverServer server;
    CoverDownloader downloader;

    SECTION ("an image arrives, and a second request is answered from the cache") {
	const auto url = server.url ("/cover.png");

	CHECK_FALSE (downloader.request (url).has_value ());

	const auto finished = waitFor (downloader, url);
	REQUIRE (finished.has_value ());
	REQUIRE (finished->path.has_value ());
	expectRedWithBlueCorner (loadCoverImage (*finished->path));

	CHECK (downloader.request (url) == finished->path);
	CHECK (server.requests == 1);
    }

    SECTION ("a redirect is followed and a WebP stays decodable") {
	const auto redirected = server.url ("/redirect");
	CHECK_FALSE (downloader.request (redirected).has_value ());
	const auto first = waitFor (downloader, redirected);
	REQUIRE (first.has_value ());
	REQUIRE (first->path.has_value ());
	expectRedWithBlueCorner (loadCoverImage (*first->path));

	const auto webp = server.url ("/cover.webp");
	CHECK_FALSE (downloader.request (webp).has_value ());
	const auto second = waitFor (downloader, webp);
	REQUIRE (second.has_value ());
	REQUIRE (second->path.has_value ());
	expectRedWithBlueCorner (loadCoverImage (*second->path));
    }

    SECTION ("an error page and a missing file leave nothing behind") {
	for (const auto* path : { "/page.html", "/nothing-here" }) {
	    const auto url = server.url (path);

	    CHECK_FALSE (downloader.request (url).has_value ());

	    const auto finished = waitFor (downloader, url);
	    REQUIRE (finished.has_value ());
	    CHECK_FALSE (finished->path.has_value ());
	    CHECK_FALSE (std::filesystem::exists (downloader.cachePath (url)));
	}
    }

    std::filesystem::remove_all (cache);
}
