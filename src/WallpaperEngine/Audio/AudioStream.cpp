#include "AudioStream.h"
#include "WallpaperEngine/Logging/Log.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace WallpaperEngine::Audio;

namespace {
/** OpenAL Soft 1.21.1 core/mixer/defs.h and alu.cpp */
constexpr uint32_t MixerFracBits = 12;
constexpr uint32_t MixerFracOne = 1 << MixerFracBits;
constexpr uint32_t MixerFracMask = MixerFracOne - 1;
constexpr uint32_t MaxPitch = 10;
constexpr size_t MaxResamplerPadding = 48;
} // namespace

int audio_read_thread (void* arg) {
    SDL_mutex* waitMutex = SDL_CreateMutex ();
    auto* stream = static_cast<AudioStream*> (arg);
    AVPacket* packet = av_packet_alloc ();
    int ret = 0;

    if (waitMutex == nullptr) {
	sLog.exception ("Cannot create mutex for audio playback waiting");
    }

    while (ret >= 0 && stream->getAudioContext ().getApplicationContext ().state.general.keepRunning
	   && stream->isInitialized ()) {
	// give the cpu some time to play the queued frames if there's enough info there
	if (stream->getQueueSize () >= MAX_QUEUE_SIZE
	    || (stream->getQueuePacketCount () > MIN_FRAMES
		&& (av_q2d (stream->getTimeBase ()) * stream->getQueueDuration () > 1.0))) {
	    SDL_LockMutex (waitMutex);
	    SDL_CondWaitTimeout (stream->getWaitCondition (), waitMutex, 10);
	    SDL_UnlockMutex (waitMutex);
	    continue;
	}

	ret = av_read_frame (stream->getFormatContext (), packet);

	if (ret == AVERROR_EOF) {
	    if (!stream->isRepeat ()) {
		break;
	    }

	    avformat_seek_file (stream->getFormatContext (), stream->getAudioStream (), 0, 0, 0, ~AVSEEK_FLAG_FRAME);
	    stream->queueLoopMarker ();
	    ret = 0;

	    continue;
	}

	if (packet->stream_index == stream->getAudioStream ()) {
	    stream->queuePacket (packet);
	} else {
	    av_packet_unref (packet);
	}
    }

    SDL_DestroyMutex (waitMutex);
    av_packet_free (&packet);
    stream->markReaderFinished ();

    return 0;
}

static int audio_read_data_callback (void* streamarg, uint8_t* buffer, int buffer_size) {
    const auto stream = static_cast<AudioStream*> (streamarg);

    if (stream->getBuffer ()->eof ()) {
	return AVERROR_EOF;
    }

    stream->getBuffer ()->read (reinterpret_cast<std::istream::char_type*> (buffer), buffer_size);

    if (stream->getBuffer ()->fail () && !stream->getBuffer ()->eof ()) {
	return AVERROR_INVALIDDATA;
    }

    return stream->getBuffer ()->gcount ();
}

int64_t audio_seek_data_callback (void* streamarg, int64_t offset, int whence) {
    const auto stream = static_cast<AudioStream*> (streamarg);

    // reset error state
    stream->getBuffer ()->clear ();

    if (whence & AVSEEK_SIZE) {
	const auto current = stream->getBuffer ()->tellg ();
	stream->getBuffer ()->seekg (0, std::ios_base::end);
	const auto end = stream->getBuffer ()->tellg ();
	stream->getBuffer ()->seekg (current, std::ios_base::beg);
	return end;
    }

    switch (whence) {
	case SEEK_CUR:
	    stream->getBuffer ()->seekg (offset, std::ios_base::cur);
	    break;
	case SEEK_SET:
	    stream->getBuffer ()->seekg (offset, std::ios_base::beg);
	    break;
	case SEEK_END:
	    stream->getBuffer ()->seekg (offset, std::ios_base::end);
	    break;
    }

    return 0;
}

AudioStream::AudioStream (AudioContext& context, const std::string& filename) : m_audioContext (context) {
    this->loadCustomContent (filename.c_str ());
}

AudioStream::AudioStream (AudioContext& context, const ReadStreamSharedPtr& buffer, bool repeat) :
    m_audioContext (context), m_repeat (repeat) {
    this->m_formatContext = avformat_alloc_context ();

    if (this->m_formatContext == nullptr) {
	sLog.exception ("Cannot allocate ffmpeg format context");
    }

    this->m_buffer = buffer;

    this->m_formatContext->pb = avio_alloc_context (
	static_cast<uint8_t*> (av_malloc (4096)), 4096, 0, this, &audio_read_data_callback, nullptr,
	&audio_seek_data_callback
    );

    if (this->m_formatContext->pb == nullptr) {
	sLog.exception ("Cannot create avio context");
    }

    this->loadCustomContent ();
}

AudioStream::AudioStream (AudioContext& audioContext, AVCodecContext* context) :
    m_audioContext (audioContext), m_context (context), m_queue (new PacketQueue) {
    this->initialize ();
}

AudioStream::~AudioStream () {
    this->stop ();

    if (this->m_audioThread != nullptr) {
	SDL_WaitThread (this->m_audioThread, nullptr);
    }

    this->m_audioThread = nullptr;

    if (this->m_swrctx != nullptr && swr_is_initialized (this->m_swrctx) == true) {
	swr_close (this->m_swrctx);
    }
    if (this->m_swrctx != nullptr) {
	swr_free (&this->m_swrctx);
    }
    if (this->m_decodePacket != nullptr) {
	av_packet_free (&this->m_decodePacket);
    }
    if (this->m_decodeFrame != nullptr) {
	av_frame_free (&this->m_decodeFrame);
    }
    if (this->m_queue != nullptr && this->m_queue->packetList != nullptr) {
#if FF_API_FIFO_OLD_API
	av_fifo_free (this->m_queue->packetList);
	this->m_queue->packetList = nullptr;
#else
	av_fifo_freep2 (&this->m_queue->packetList);
#endif /* FF_API_FIFO_OLD_API */
    }

    delete this->m_queue;

    if (this->m_formatContext != nullptr) {
	avformat_free_context (this->m_formatContext);
    }

    if (this->m_context != nullptr) {
	avcodec_free_context (&this->m_context);
    }
}

void AudioStream::loadCustomContent (const char* filename) {
    if (avformat_open_input (&this->m_formatContext, filename, nullptr, nullptr) != 0) {
	sLog.exception ("Cannot open audio file: ", filename);
    }
    if (avformat_find_stream_info (this->m_formatContext, nullptr) < 0) {
	sLog.exception ("Cannot determine file format: ", filename);
    }

    for (unsigned int i = 0; i < this->m_formatContext->nb_streams; i++) {
	if (this->m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
	    && this->m_audioStream == NO_AUDIO_STREAM) {
	    this->m_audioStream = i;
	}
    }

    if (this->m_audioStream == NO_AUDIO_STREAM) {
	sLog.exception ("Cannot find an audio stream in file ", filename);
    }

    const AVCodec* aCodec
	= avcodec_find_decoder (this->m_formatContext->streams[this->m_audioStream]->codecpar->codec_id);

    if (aCodec == nullptr) {
	sLog.exception ("Cannot initialize audio decoder for file: ", filename);
    }

    AVCodecContext* avCodecContext = avcodec_alloc_context3 (aCodec);

    if (avcodec_parameters_to_context (avCodecContext, this->m_formatContext->streams[this->m_audioStream]->codecpar)
	!= 0) {
	sLog.exception ("Cannot initialize audio decoder parameters");
    }

    avcodec_open2 (avCodecContext, aCodec, nullptr);

    this->m_context = avCodecContext;
    this->m_queue = new PacketQueue;

    this->initialize ();

    this->m_audioThread = SDL_CreateThread (audio_read_thread, filename, this);
}

void AudioStream::initialize () {
#if FF_API_FIFO_OLD_API
    this->m_queue->packetList = av_fifo_alloc (sizeof (MyAVPacketList));
#else
    this->m_queue->packetList = av_fifo_alloc2 (1, sizeof (MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
#endif

#if FF_API_OLD_CHANNEL_LAYOUT
    // the mixer reads frames of the device's width, so swr has to write that many channels
    const int64_t out_channel_layout = av_get_default_channel_layout (this->m_audioContext.getChannels ());

    this->m_swrctx = swr_alloc_set_opts (
	nullptr, out_channel_layout, AV_SAMPLE_FMT_FLT, this->getContext ()->sample_rate,
	this->getContext ()->channel_layout, this->getContext ()->sample_fmt, this->getContext ()->sample_rate, 0,
	nullptr
    );
#else
    // the mixer reads frames of the device's width, so swr has to write that many channels
    AVChannelLayout out_channel_layout;
    av_channel_layout_default (&out_channel_layout, this->m_audioContext.getChannels ());

    // only the sample format and the layout change here, the rate is converted in read () like OpenAL does
    swr_alloc_set_opts2 (
	&this->m_swrctx, &out_channel_layout, AV_SAMPLE_FMT_FLT, this->m_context->sample_rate,
	&this->m_context->ch_layout, this->m_context->sample_fmt, this->m_context->sample_rate, 0, nullptr
    );
#endif

    if (this->m_swrctx == nullptr) {
	sLog.exception ("Cannot initialize swrctx for audio resampling");
    }

    // mono goes to both channels unchanged, the mixer pans it with per channel gains like OpenAL does for WE
    if (this->getSourceChannels () == 1 && this->m_audioContext.getChannels () == 2) {
	constexpr double duplicate[2] = { 1.0, 1.0 };

	swr_set_matrix (this->m_swrctx, duplicate, 1);
    }

    if (swr_init (this->m_swrctx) < 0) {
	sLog.exception ("Failed to initialize the resampling context.");
    }

    this->m_channels = this->m_audioContext.getChannels ();

    // CalcNonAttnSourceParams/CalcAttnSourceParams: pitch 1 times buffer rate / device rate, rounded to the 12 bit
    // fixed point step (fastf2u is cvtss2si in the DLL, sub_180190400 and sub_1801920C0), at most 10x
    const float pitch = static_cast<float> (this->m_context->sample_rate)
	/ static_cast<float> (this->m_audioContext.getSampleRate ());

    if (pitch > static_cast<float> (MaxPitch)) {
	this->m_step = MaxPitch << MixerFracBits;
    } else {
	this->m_step = std::max<uint32_t> (static_cast<uint32_t> (std::lrint (pitch * MixerFracOne)), 1);
    }

    this->m_queue->mutex = SDL_CreateMutex ();
    this->m_queue->wait = SDL_CreateCond ();

    this->m_decodeFrame = av_frame_alloc ();
    this->m_decodePacket = av_packet_alloc ();

    if (!this->m_decodeFrame) {
	sLog.exception ("Could not allocate AVFrame.\n");
    }
    if (!this->m_decodePacket) {
	sLog.exception ("Could not allocate AVPacket.\n");
    }

    this->m_initialized = true;
}

void AudioStream::queuePacket (AVPacket* pkt) {
    AVPacket* clone = av_packet_alloc ();

    if (clone == nullptr) {
	av_packet_unref (clone);
	return;
    }

    av_packet_move_ref (clone, pkt);

    SDL_LockMutex (this->m_queue->mutex);
    const bool gotQueued = this->doQueue (clone);
    SDL_UnlockMutex (this->m_queue->mutex);

    if (!gotQueued) {
	av_packet_free (&pkt);
    }
}

bool AudioStream::doQueue (AVPacket* pkt) {
    MyAVPacketList entry { pkt };

#if FF_API_FIFO_OLD_API
    if (av_fifo_space (this->m_queue->packetList) < static_cast<int> (sizeof (entry))) {
	if (av_fifo_grow (this->m_queue->packetList, sizeof (entry)) < 0) {
	    return false;
	}
    }

    av_fifo_generic_write (this->m_queue->packetList, &entry, sizeof (entry), nullptr);
#else
    if (av_fifo_write (this->m_queue->packetList, &entry, 1) < 0) {
	return false;
    }
#endif

    this->m_queue->nb_packets++;
    this->m_queue->size += entry.packet->size + sizeof (entry);
    this->m_queue->duration += entry.packet->duration;

    return true;
}

void AudioStream::queueLoopMarker () {
    AVPacket* marker = av_packet_alloc ();

    if (marker == nullptr) {
	return;
    }

    // empty packet, recognised by its stream index
    marker->stream_index = NO_AUDIO_STREAM;

    SDL_LockMutex (this->m_queue->mutex);
    const bool gotQueued = this->doQueue (marker);
    SDL_UnlockMutex (this->m_queue->mutex);

    if (!gotQueued) {
	av_packet_free (&marker);
    }
}

void AudioStream::markReaderFinished () { this->m_readerFinished = true; }

AudioStream::Dequeued AudioStream::dequeuePacket () {
    MyAVPacketList entry {};
    // read before the queue, the reader sets it after queueing its last packet
    const bool finished = this->m_readerFinished || !this->m_initialized;

    SDL_LockMutex (this->m_queue->mutex);

#if FF_API_FIFO_OLD_API
    int ret = -1;

    if (av_fifo_size (this->m_queue->packetList) >= static_cast<int> (sizeof (entry))) {
	ret = av_fifo_generic_read (this->m_queue->packetList, &entry, sizeof (entry), nullptr);
    }
#else
    const int ret = av_fifo_read (this->m_queue->packetList, &entry, 1);
#endif

    if (ret >= 0) {
	this->m_queue->nb_packets--;
	this->m_queue->size -= entry.packet->size + sizeof (entry);
	this->m_queue->duration -= entry.packet->duration;
    }

    SDL_UnlockMutex (this->m_queue->mutex);

    if (ret < 0) {
	return finished ? Dequeued::Finished : Dequeued::Empty;
    }

    av_packet_unref (this->m_decodePacket);
    av_packet_move_ref (this->m_decodePacket, entry.packet);
    av_packet_free (&entry.packet);

    return Dequeued::Packet;
}

AVCodecContext* AudioStream::getContext () const { return this->m_context; }

AVFormatContext* AudioStream::getFormatContext () const { return this->m_formatContext; }

int AudioStream::getAudioStream () const { return this->m_audioStream; }

bool AudioStream::isInitialized () const { return this->m_initialized; }

double AudioStream::getDuration () const {
    if (this->m_formatContext == nullptr) {
	return 0.0;
    }

    if (this->m_formatContext->duration > 0) {
	return static_cast<double> (this->m_formatContext->duration) / AV_TIME_BASE;
    }

    if (this->m_audioStream == NO_AUDIO_STREAM) {
	return 0.0;
    }

    const AVStream* stream = this->m_formatContext->streams[this->m_audioStream];

    return stream->duration > 0 ? static_cast<double> (stream->duration) * av_q2d (stream->time_base) : 0.0;
}

int AudioStream::getSourceChannels () const {
#if FF_API_OLD_CHANNEL_LAYOUT
    return this->m_context->channels;
#else
    return this->m_context->ch_layout.nb_channels;
#endif
}

void AudioStream::setRepeat (const bool newRepeat) { this->m_repeat = newRepeat; }

bool AudioStream::isRepeat () const { return this->m_repeat; }

ReadStreamSharedPtr& AudioStream::getBuffer () { return this->m_buffer; }

SDL_cond* AudioStream::getWaitCondition () const { return this->m_queue->wait; }

size_t AudioStream::getQueueSize () const { return this->m_queue->size; }

int AudioStream::getQueuePacketCount () const { return this->m_queue->nb_packets; }

AVRational AudioStream::getTimeBase () const {
    if (this->m_audioStream == NO_AUDIO_STREAM) {
	return { 0, 0 };
    }

    return this->m_formatContext->streams[this->m_audioStream]->time_base;
}

int64_t AudioStream::getQueueDuration () const { return this->m_queue->duration; }

void AudioStream::stop () {
    if (!this->isInitialized ()) {
	return;
    }

    // stop the threads running
    this->m_initialized = false;
}

bool AudioStream::convertFrame () {
    const int samples = this->m_decodeFrame->nb_samples;

    if (samples <= 0) {
	return false;
    }

    const size_t offset = this->m_source.size ();

    this->m_source.resize (offset + static_cast<size_t> (samples) * this->m_channels);

    auto* out = reinterpret_cast<uint8_t*> (this->m_source.data () + offset);
    const int converted
	= swr_convert (this->m_swrctx, &out, samples, const_cast<const uint8_t**> (this->m_decodeFrame->data), samples);

    this->m_source.resize (offset + static_cast<size_t> (std::max (converted, 0)) * this->m_channels);

    if (converted < 0) {
	sLog.error ("swr_convert_error.");
    }

    return converted > 0;
}

void AudioStream::flushConverter () {
    const int samples = swr_get_out_samples (this->m_swrctx, 0);

    if (samples <= 0) {
	return;
    }

    const size_t offset = this->m_source.size ();

    this->m_source.resize (offset + static_cast<size_t> (samples) * this->m_channels);

    auto* out = reinterpret_cast<uint8_t*> (this->m_source.data () + offset);
    const int converted = swr_convert (this->m_swrctx, &out, samples, nullptr, 0);

    this->m_source.resize (offset + static_cast<size_t> (std::max (converted, 0)) * this->m_channels);
}

int AudioStream::decodeSource () {
    // runs on the audio callback, so it never waits
    while (this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
	const int ret = avcodec_receive_frame (this->getContext (), this->m_decodeFrame);

	if (ret == 0) {
	    if (this->convertFrame ()) {
		return 1;
	    }

	    continue;
	}

	if (ret == AVERROR_EOF) {
	    if (this->m_loopDrain) {
		avcodec_flush_buffers (this->getContext ());
		this->m_loopDrain = false;
		continue;
	    }

	    this->flushConverter ();
	    return -1;
	}

	if (ret != AVERROR (EAGAIN)) {
	    return -1;
	}

	switch (this->dequeuePacket ()) {
	    case Dequeued::Empty:
		return 0;
	    case Dequeued::Finished:
		avcodec_send_packet (this->getContext (), nullptr);
		continue;
	    case Dequeued::Packet:
		break;
	}

	if (this->m_decodePacket->stream_index == NO_AUDIO_STREAM) {
	    avcodec_send_packet (this->getContext (), nullptr);
	    this->m_loopDrain = true;
	    continue;
	}

	avcodec_send_packet (this->getContext (), this->m_decodePacket);
	av_packet_unref (this->m_decodePacket);
    }

    return -1;
}

void AudioStream::fillSource (size_t frames) {
    while (!this->m_sourceEnded && this->m_source.size () / this->m_channels < this->m_sourcePos + frames) {
	const int status = this->decodeSource ();

	if (status < 0) {
	    this->m_sourceEnded = true;
	} else if (status == 0) {
	    return;
	}
    }
}

const float* AudioStream::sourceFrame (size_t frame) const {
    // LoadBufferQueue stops at the queue's end and the rest of the source line repeats the last sample
    const size_t available = this->m_source.size () / this->m_channels;

    return this->m_source.data () + std::min (frame, available - 1) * this->m_channels;
}

int AudioStream::read (float* out, const int frames) {
    if (this->m_ended) {
	return -1;
    }

    // the frames this update reads, plus the next one the last of them interpolates towards
    const uint64_t last = (static_cast<uint64_t> (frames - 1) * this->m_step + this->m_sourceFrac) >> MixerFracBits;

    this->fillSource (static_cast<size_t> (last) + 2);

    const size_t available = this->m_source.size () / this->m_channels;

    if (available == 0) {
	if (this->m_sourceEnded) {
	    this->m_ended = true;
	    return -1;
	}

	return 0;
    }

    int written = 0;

    for (; written < frames; written++) {
	// a voice whose file is over keeps mixing the held last frame until its update is done
	const bool past = this->m_sourcePos + 1 >= available;

	if (past && !this->m_sourceEnded) {
	    break;
	}

	const float* first = this->sourceFrame (this->m_sourcePos);
	const float* second = this->sourceFrame (this->m_sourcePos + 1);
	const float mu = static_cast<float> (this->m_sourceFrac) * (1.0f / MixerFracOne);

	for (int channel = 0; channel < this->m_channels; channel++) {
	    out[written * this->m_channels + channel] = first[channel] + (second[channel] - first[channel]) * mu;
	}

	this->m_sourceFrac += this->m_step;
	this->m_sourcePos += this->m_sourceFrac >> MixerFracBits;
	this->m_sourceFrac &= MixerFracMask;
    }

    // OpenAL knows where its queue ends: a file running out right at the end of an update ends the voice there too
    if (this->m_sourcePos >= available) {
	this->fillSource (1);
    }

    if (this->m_sourceEnded && this->m_sourcePos >= this->m_source.size () / this->m_channels) {
	this->m_ended = true;
    }

    // keep the frame before the position (a stopping voice's history) and what the next update reads
    if (this->m_sourcePos > 4096) {
	const size_t drop = std::min (this->m_sourcePos - 1, available - 1);

	this->m_source.erase (
	    this->m_source.begin (), this->m_source.begin () + static_cast<long> (drop * this->m_channels)
	);
	this->m_sourcePos -= drop;
    }

    return written;
}

bool AudioStream::ended () const { return this->m_ended; }

std::vector<int> AudioStream::passes (const int frames) const {
    constexpr uint64_t BufferLineSize = 1024;
    std::vector<int> result;
    uint32_t frac = this->m_sourceFrac;

    for (int done = 0; done < frames;) {
	uint64_t count = static_cast<uint64_t> (frames - done);

	if (this->m_step > MixerFracOne
	    && ((count * this->m_step + frac) >> MixerFracBits) + MaxResamplerPadding
		> BufferLineSize + MaxResamplerPadding) {
	    // a multiple of 4 for the SIMD mixers
	    const uint64_t fits = ((BufferLineSize << MixerFracBits) - frac) / this->m_step;

	    if (fits < count) {
		count = fits & ~uint64_t { 3 };
	    }
	}

	result.push_back (static_cast<int> (count));
	frac = static_cast<uint32_t> ((frac + this->m_step * count) & MixerFracMask);
	done += static_cast<int> (count);
    }

    return result;
}

void AudioStream::stopTail (float* out, const int frames) {
    constexpr size_t Padding = MaxResamplerPadding >> 1;
    const int channels = this->m_channels;

    this->fillSource (Padding);

    if (this->m_source.empty ()) {
	std::fill_n (out, static_cast<size_t> (frames) * channels, 0.0f);
	return;
    }

    std::vector<float> line (Padding + 1);

    for (int channel = 0; channel < channels; channel++) {
	// mPrevSamples: the frame before the position (zeros before a voice's first update), then the next Padding
	const float before = this->m_sourcePos > 0 ? this->sourceFrame (this->m_sourcePos - 1)[channel] : 0.0f;
	size_t count = 0;
	size_t quietest = 0;

	for (size_t i = 0; i < Padding; i++) {
	    line[i] = this->sourceFrame (this->m_sourcePos + i)[channel];

	    if (std::abs (line[i]) < std::abs (line[quietest])) {
		quietest = i;
	    }
	}

	count = quietest;

	const float held = count > 0 ? line[count - 1] : before;

	std::fill (line.begin () + static_cast<long> (count), line.end (), held);

	uint32_t frac = this->m_sourceFrac;
	size_t pos = 0;

	for (int i = 0; i < frames; i++) {
	    const float first = line[std::min (pos, Padding)];
	    const float second = line[std::min (pos + 1, Padding)];

	    out[i * channels + channel]
		= first + (second - first) * (static_cast<float> (frac) * (1.0f / MixerFracOne));

	    frac += this->m_step;
	    pos += frac >> MixerFracBits;
	    frac &= MixerFracMask;
	}
    }

    this->m_ended = true;
}

AudioContext& AudioStream::getAudioContext () const { return this->m_audioContext; }
