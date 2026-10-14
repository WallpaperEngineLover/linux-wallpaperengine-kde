#pragma once

#include <atomic>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL.h>
#include <SDL_thread.h>

#include "WallpaperEngine/Audio/AudioContext.h"

#ifndef FF_API_FIFO_OLD_API
#define FF_API_FIFO_OLD_API (LIBAVUTIL_VERSION_MAJOR < 59)
#endif
#ifndef FF_API_OLD_CHANNEL_LAYOUT
#define FF_API_OLD_CHANNEL_LAYOUT (LIBAVUTIL_VERSION_MAJOR < 59)
#endif

#define MAX_QUEUE_SIZE (5 * 1024 * 1024)
#define MIN_FRAMES (25)
#define NO_AUDIO_STREAM (-1)

namespace WallpaperEngine::Audio {
class AudioContext;

using namespace WallpaperEngine::FileSystem;

class AudioStream {
public:
    AudioStream (AudioContext& context, const std::string& filename);
    /**
     * @param repeat Set before the reader thread starts, a short file could otherwise be read to its end before a later
     * setRepeat () call
     */
    AudioStream (AudioContext& context, const ReadStreamSharedPtr& buffer, bool repeat = false);
    AudioStream (AudioContext& audioContext, AVCodecContext* context);
    ~AudioStream ();

    void queuePacket (AVPacket* pkt);

    enum class Dequeued { Packet, Empty, Finished };

    /**
     * Takes the next packet off the queue into m_decodePacket without waiting
     *
     * @return Empty when the reader hasn't queued more yet, Finished once it never will
     */
    Dequeued dequeuePacket ();

    /**
     * Called by the file reader thread when a repeating stream starts over, the decoder drains up to the marker
     */
    void queueLoopMarker ();

    /**
     * Called by the file reader thread when it will not queue any more packets
     */
    void markReaderFinished ();

    [[nodiscard]] AudioContext& getAudioContext () const;

    [[nodiscard]] AVCodecContext* getContext () const;
    [[nodiscard]] AVFormatContext* getFormatContext () const;
    [[nodiscard]] int getAudioStream () const;
    [[nodiscard]] bool isInitialized () const;
    /**
     * @return Length of the file in seconds, 0 if the container doesn't say
     */
    [[nodiscard]] double getDuration () const;
    /**
     * @return Channel count of the file itself, before resampling to the driver's layout
     */
    [[nodiscard]] int getSourceChannels () const;
    void setRepeat (bool newRepeat = true);
    [[nodiscard]] bool isRepeat () const;
    void stop ();
    [[nodiscard]] ReadStreamSharedPtr& getBuffer ();
    /**
     * @return The SDL_cond used to signal waiting for data
     */
    [[nodiscard]] SDL_cond* getWaitCondition () const;
    [[nodiscard]] size_t getQueueSize () const;
    [[nodiscard]] int getQueuePacketCount () const;
    /**
     * @return The duration (in seconds) of the queued data to be played
     */
    [[nodiscard]] int64_t getQueueDuration () const;
    [[nodiscard]] AVRational getTimeBase () const;

    /**
     * Plays the stream at the driver's rate the way an OpenAL Soft 1.21.1 source plays a file at its own rate (WE hands
     * every sound file to OpenAL as is, mediaextensions64.dll sub_18000D530): linear interpolation over a 12 bit fixed
     * point position, the step rounded from file rate / device rate. Past the end the last frame is held, like the
     * padding OpenAL loads after a queue's end.
     *
     * @param out Interleaved float frames in the driver's channel layout
     * @return Frames written, 0 when nothing is decoded yet, < 0 once the voice is over
     */
    int read (float* out, int frames);

    /**
     * How OpenAL splits the next update of this many frames into mixing passes (Voice::mix): a step above 1 reads more
     * source frames than it writes, and one pass loads at most BufferLineSize of them
     */
    [[nodiscard]] std::vector<int> passes (int frames) const;

    /** Whether the voice played past the file's end, OpenAL then stops it after the current update */
    [[nodiscard]] bool ended () const;

    /**
     * What a stopping voice mixes in its last update when it has no buffer left (alSourceStop, or the natural end): up
     * to 24 of the upcoming frames, cut per channel before the one closest to zero and held from there (voice.cpp)
     */
    void stopTail (float* out, int frames);

private:
    /**
     * Initializes ffmpeg to read the given file
     */
    void loadCustomContent (const char* filename = nullptr);
    /** Converts the decoded frame to float in the driver's channel layout at the file's rate, onto m_source */
    bool convertFrame ();
    /** Converts what the converter still holds once the decoder is drained */
    void flushConverter ();
    /**
     * Decodes the next frame onto m_source without waiting
     *
     * @return 1 when frames were added, 0 when nothing is queued yet, -1 at the end or on an error
     */
    int decodeSource ();
    /** Makes sure m_source holds this many frames past the read position, unless the file ends first */
    void fillSource (size_t frames);
    [[nodiscard]] const float* sourceFrame (size_t frame) const;
    bool doQueue (AVPacket* pkt);
    /**
     * Initializes queues and ffmpeg resampling
     */
    void initialize ();

    SwrContext* m_swrctx = nullptr;
    AudioContext& m_audioContext;
    bool m_initialized = false;
    bool m_repeat = false;
    /** Set once the reader thread has exited, so nothing waits for packets that will never come */
    std::atomic<bool> m_readerFinished = false;
    /** The codec context that contains the original audio format information */
    AVCodecContext* m_context = nullptr;
    AVFormatContext* m_formatContext = nullptr;
    int m_audioStream = NO_AUDIO_STREAM;
    ReadStreamSharedPtr m_buffer = nullptr;

    struct MyAVPacketList {
	AVPacket* packet;
    };

    /** decoded frames at the file's rate, interleaved; m_sourcePos and m_sourceFrac are the voice position in it */
    std::vector<float> m_source;
    int m_channels = 0;
    size_t m_sourcePos = 0;
    uint32_t m_sourceFrac = 0;
    uint32_t m_step = 0;
    bool m_sourceEnded = false;
    bool m_ended = false;

    AVPacket* m_decodePacket = nullptr;
    AVFrame* m_decodeFrame = nullptr;
    bool m_loopDrain = false;

    struct PacketQueue {
#if FF_API_FIFO_OLD_API
	AVFifoBuffer* packetList = nullptr;
#else
	AVFifo* packetList = nullptr;
#endif
	int nb_packets = 0;
	size_t size = 0;
	int64_t duration = 0;
	SDL_mutex* mutex = nullptr;
	SDL_cond* wait = nullptr;
    }* m_queue {};

    SDL_Thread* m_audioThread = nullptr;
};
} // namespace WallpaperEngine::Audio
