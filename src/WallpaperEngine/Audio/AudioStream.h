#pragma once

#include <atomic>
#include <string>
#include <thread>
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
    // repeat is set before the reader starts, a short file could be read to its end before a setRepeat () call
    AudioStream (AudioContext& context, const ReadStreamSharedPtr& buffer, bool repeat = false);
    AudioStream (AudioContext& audioContext, AVCodecContext* context);
    ~AudioStream ();

    void queuePacket (AVPacket* pkt);

    enum class Dequeued { Packet, Empty, Finished };

    // next packet into m_decodePacket without waiting; Empty if nothing is queued yet, Finished once nothing will be
    Dequeued dequeuePacket ();

    // from the reader thread when a repeating stream starts over
    void queueLoopMarker ();

    // from the reader thread once it won't queue more packets
    void markReaderFinished ();

    [[nodiscard]] AudioContext& getAudioContext () const;

    [[nodiscard]] AVCodecContext* getContext () const;
    [[nodiscard]] AVFormatContext* getFormatContext () const;
    [[nodiscard]] int getAudioStream () const;
    [[nodiscard]] bool isInitialized () const;
    // seconds, 0 if the container doesn't say
    [[nodiscard]] double getDuration () const;
    // before resampling to the driver's layout
    [[nodiscard]] int getSourceChannels () const;
    void setRepeat (bool newRepeat = true);
    [[nodiscard]] bool isRepeat () const;
    void stop ();
    [[nodiscard]] ReadStreamSharedPtr& getBuffer ();
    [[nodiscard]] SDL_cond* getWaitCondition () const;
    [[nodiscard]] size_t getQueueSize () const;
    [[nodiscard]] int getQueuePacketCount () const;
    // seconds
    [[nodiscard]] int64_t getQueueDuration () const;
    [[nodiscard]] AVRational getTimeBase () const;

    // plays at the driver's rate like an OpenAL Soft 1.21.1 source (WE hands files to OpenAL as is, sub_18000D530):
    // linear interpolation, 12 bit fixed point step, last frame held past the end returns frames written, 0 if nothing
    // is decoded yet, < 0 once the voice is over
    int read (float* out, int frames);

    // how OpenAL splits the next update into mixing passes (Voice::mix), at most BufferLineSize source frames each
    [[nodiscard]] std::vector<int> passes (int frames) const;

    /** Whether the voice played past the file's end, OpenAL then stops it after the current update */
    [[nodiscard]] bool ended () const;

    // a stopping voice's last update without buffers: up to 24 upcoming frames, cut per channel before the one closest
    // to zero and held (voice.cpp)
    void stopTail (float* out, int frames);

private:
    void loadCustomContent (const char* filename = nullptr);
    /** Converts the decoded frame to float in the driver's channel layout at the file's rate, onto m_source */
    bool convertFrame ();
    /** Converts what the converter still holds once the decoder is drained */
    void flushConverter ();
    // decodes the next frame onto m_source without waiting: 1 added, 0 nothing queued, -1 end or error
    int decodeSource ();
    /** Makes sure m_source holds this many frames past the read position, unless the file ends first */
    void fillSource (size_t frames);
    [[nodiscard]] const float* sourceFrame (size_t frame) const;
    bool doQueue (AVPacket* pkt);
    void initialize ();

    SwrContext* m_swrctx = nullptr;
    AudioContext& m_audioContext;
    std::atomic<bool> m_initialized = false;
    std::atomic<bool> m_repeat = false;
    /** Set once the reader thread has exited, so nothing waits for packets that will never come */
    std::atomic<bool> m_readerFinished = false;
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

    // std::thread, not SDL_Thread: SDL_WaitThread doesn't join anymore once SDL_Quit ran (sdl2-compat)
    std::thread m_audioThread;
};
} // namespace WallpaperEngine::Audio
