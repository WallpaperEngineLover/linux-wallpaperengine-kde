#pragma once

#include <atomic>
#include <string>

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
    [[nodiscard]] SDL_mutex* getMutex () const;

    /**
     * Reads a frame from the audio stream, resamples it to the driver's settings
     * and returns the data ready to be played
     *
     * @return The amount of bytes available, 0 when nothing is decoded yet or < 0 at the end or on error
     */
    int decodeFrame (uint8_t* audioBuffer, int bufferSize);

private:
    /**
     * Initializes ffmpeg to read the given file
     */
    void loadCustomContent (const char* filename = nullptr);
    /**
     * Converts the audio frame from the original format to one supported by the audio driver
     */
    int resampleAudio (uint8_t* out_buf, const int out_size);
    /** Converts what the resampler still holds once the decoder is drained */
    int flushResampler (uint8_t* out_buf, int out_size);
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
	SDL_cond* cond = nullptr;
    }* m_queue {};

    SDL_Thread* m_audioThread = nullptr;
};
} // namespace WallpaperEngine::Audio
