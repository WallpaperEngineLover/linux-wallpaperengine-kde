#include "SogLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace WallpaperEngine::Splat {
namespace {
    constexpr float SH_C0 = 0.28209479177387814f;

    struct CodecContextDeleter {
	void operator() (AVCodecContext* ctx) const { avcodec_free_context (&ctx); }
    };
    struct PacketDeleter {
	void operator() (AVPacket* packet) const { av_packet_free (&packet); }
    };
    struct FrameDeleter {
	void operator() (AVFrame* frame) const { av_frame_free (&frame); }
    };
    struct SwsDeleter {
	void operator() (SwsContext* ctx) const { sws_freeContext (ctx); }
    };

    std::vector<float> readCodebook (const nlohmann::json& section, const char* name) {
	auto codebook = section.at ("codebook").get<std::vector<float>> ();

	if (codebook.size () != 256) {
	    throw std::runtime_error (std::string ("SOG ") + name + " codebook must have 256 entries");
	}

	return codebook;
    }

    DecodedImage readImage (const FileReader& readFile, const nlohmann::json& section, size_t fileIndex) {
	const auto& files = section.at ("files");

	if (!files.is_array () || files.size () <= fileIndex) {
	    throw std::runtime_error ("SOG meta lists too few files for a section");
	}

	return decodeWebP (readFile (files.at (fileIndex).get<std::string> ()));
    }

    inline float unpackByte (uint8_t value) { return static_cast<float> (value) / 255.0f; }

    inline uint32_t packColor (float r, float g, float b, float a) {
	const auto toByte = [] (float value) {
	    return static_cast<uint32_t> (std::lround (std::clamp (value, 0.0f, 1.0f) * 255.0f));
	};

	return toByte (r) | (toByte (g) << 8) | (toByte (b) << 16) | (toByte (a) << 24);
    }
} // namespace

DecodedImage decodeWebP (const std::string& bytes) {
    const AVCodec* codec = avcodec_find_decoder (AV_CODEC_ID_WEBP);

    if (codec == nullptr) {
	throw std::runtime_error ("This FFmpeg build has no WebP decoder");
    }

    std::unique_ptr<AVCodecContext, CodecContextDeleter> context (avcodec_alloc_context3 (codec));

    if (!context || avcodec_open2 (context.get (), codec, nullptr) < 0) {
	throw std::runtime_error ("Cannot open the FFmpeg WebP decoder");
    }

    std::unique_ptr<AVPacket, PacketDeleter> packet (av_packet_alloc ());

    // av_new_packet allocates the input padding the decoders require
    if (!packet || av_new_packet (packet.get (), static_cast<int> (bytes.size ())) < 0) {
	throw std::runtime_error ("Cannot allocate a WebP packet");
    }

    std::memcpy (packet->data, bytes.data (), bytes.size ());

    std::unique_ptr<AVFrame, FrameDeleter> frame (av_frame_alloc ());

    if (avcodec_send_packet (context.get (), packet.get ()) < 0 || avcodec_send_packet (context.get (), nullptr) < 0
	|| avcodec_receive_frame (context.get (), frame.get ()) < 0) {
	throw std::runtime_error ("Cannot decode a WebP image");
    }

    DecodedImage image;
    image.width = frame->width;
    image.height = frame->height;
    image.rgba.resize (static_cast<size_t> (image.width) * image.height * 4);

    std::unique_ptr<SwsContext, SwsDeleter> scaler (sws_getContext (
	frame->width, frame->height, static_cast<AVPixelFormat> (frame->format), frame->width, frame->height,
	AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr
    ));

    if (!scaler) {
	throw std::runtime_error ("Cannot convert the decoded WebP to RGBA");
    }

    uint8_t* destination[4] = { image.rgba.data (), nullptr, nullptr, nullptr };
    int destinationStride[4] = { image.width * 4, 0, 0, 0 };

    sws_scale (scaler.get (), frame->data, frame->linesize, 0, frame->height, destination, destinationStride);

    return image;
}

SplatCloud loadSog (const std::string& metaText, const FileReader& readFile) {
    const auto meta = nlohmann::json::parse (metaText);

    if (meta.value ("version", 0) != 2) {
	throw std::runtime_error ("Only SOG version 2 files are supported");
    }

    const uint32_t count = meta.at ("count").get<uint32_t> ();
    const auto& means = meta.at ("means");
    const auto mins = means.at ("mins").get<std::vector<float>> ();
    const auto maxs = means.at ("maxs").get<std::vector<float>> ();

    if (mins.size () != 3 || maxs.size () != 3) {
	throw std::runtime_error ("SOG means mins/maxs must have 3 entries");
    }

    const auto& scalesMeta = meta.at ("scales");
    const auto& colorMeta = meta.at ("sh0");
    const auto scaleCodebook = readCodebook (scalesMeta, "scales");
    const auto colorCodebook = readCodebook (colorMeta, "sh0");

    const DecodedImage meansLow = readImage (readFile, means, 0);
    const DecodedImage meansHigh = readImage (readFile, means, 1);
    const DecodedImage quats = readImage (readFile, meta.at ("quats"), 0);
    const DecodedImage scales = readImage (readFile, scalesMeta, 0);
    const DecodedImage colors = readImage (readFile, colorMeta, 0);

    for (const DecodedImage* image : { &meansHigh, &quats, &scales, &colors }) {
	if (image->width != meansLow.width || image->height != meansLow.height) {
	    throw std::runtime_error ("SOG images do not all have the same dimensions");
	}
    }

    if (static_cast<uint64_t> (meansLow.width) * meansLow.height < count) {
	throw std::runtime_error ("SOG images are too small for the declared splat count");
    }

    SplatCloud cloud;
    cloud.count = count;
    cloud.textureWidth = static_cast<uint32_t> (meansLow.width);
    cloud.textureHeight = static_cast<uint32_t> ((count + cloud.textureWidth - 1) / cloud.textureWidth);

    const size_t texels = static_cast<size_t> (cloud.textureWidth) * cloud.textureHeight;
    cloud.centerAndColor.assign (texels * 4, 0.0f);
    cloud.rotation.assign (texels * 4, 0.0f);
    cloud.scale.assign (texels * 4, 0.0f);

    const float normalizer = std::sqrt (2.0f);

    for (uint32_t i = 0; i < count; i++) {
	const uint8_t* low = &meansLow.rgba[static_cast<size_t> (i) * 4];
	const uint8_t* high = &meansHigh.rgba[static_cast<size_t> (i) * 4];
	const uint8_t* quat = &quats.rgba[static_cast<size_t> (i) * 4];
	const uint8_t* scale = &scales.rgba[static_cast<size_t> (i) * 4];
	const uint8_t* color = &colors.rgba[static_cast<size_t> (i) * 4];

	float* centerAndColor = &cloud.centerAndColor[static_cast<size_t> (i) * 4];

	for (int axis = 0; axis < 3; axis++) {
	    const float normalized = static_cast<float> (low[axis] + high[axis] * 256) / 65535.0f;
	    const float logSpace = mins[axis] + (maxs[axis] - mins[axis]) * normalized;
	    centerAndColor[axis] = std::copysign (std::expm1 (std::fabs (logSpace)), logSpace);
	}

	const uint32_t packed = packColor (
	    0.5f + colorCodebook[color[0]] * SH_C0, 0.5f + colorCodebook[color[1]] * SH_C0,
	    0.5f + colorCodebook[color[2]] * SH_C0, unpackByte (color[3])
	);
	std::memcpy (&centerAndColor[3], &packed, sizeof (packed));

	// three quantized components plus the index of the dropped (largest) one, in (w, x, y, z) order
	const float a = (unpackByte (quat[0]) - 0.5f) * normalizer;
	const float b = (unpackByte (quat[1]) - 0.5f) * normalizer;
	const float c = (unpackByte (quat[2]) - 0.5f) * normalizer;
	const float d = std::sqrt (std::max (0.0f, 1.0f - (a * a + b * b + c * c)));

	float x, y, z, w;

	switch (quat[3] - 252) {
	    case 0: x = a; y = b; z = c; w = d; break;
	    case 1: x = d; y = b; z = c; w = a; break;
	    case 2: x = b; y = d; z = c; w = a; break;
	    case 3: x = b; y = c; z = d; w = a; break;
	    default: x = 0.0f; y = 0.0f; z = 0.0f; w = 1.0f; break;
	}

	const float length = std::sqrt (x * x + y * y + z * z + w * w);
	const float inverseLength = length > 0.0f ? 1.0f / length : 1.0f;
	float* rotation = &cloud.rotation[static_cast<size_t> (i) * 4];
	rotation[0] = x * inverseLength;
	rotation[1] = y * inverseLength;
	rotation[2] = z * inverseLength;
	rotation[3] = w * inverseLength;

	float* scaleOut = &cloud.scale[static_cast<size_t> (i) * 4];
	scaleOut[0] = std::exp (scaleCodebook[scale[0]]);
	scaleOut[1] = std::exp (scaleCodebook[scale[1]]);
	scaleOut[2] = std::exp (scaleCodebook[scale[2]]);
    }

    return cloud;
}
} // namespace WallpaperEngine::Splat
