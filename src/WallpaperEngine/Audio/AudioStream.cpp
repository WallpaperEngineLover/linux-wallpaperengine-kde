#include "AudioStream.h"
#include "WallpaperEngine/Logging/Log.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace WallpaperEngine::Audio;

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
	    avformat_seek_file (stream->getFormatContext (), stream->getAudioStream (), 0, 0, 0, ~AVSEEK_FLAG_FRAME);
	    avcodec_flush_buffers (stream->getContext ());

	    // ensure the thread is not killed if audio has to be looped
	    if (stream->isRepeat ()) {
		ret = 0;
	    }

	    continue;
	}

	// TODO: PROPERLY IMPLEMENT THIS
	if (packet->stream_index == stream->getAudioStream ()) {
	    stream->queuePacket (packet);
	} else {
	    av_packet_unref (packet);
	}
    }

    SDL_DestroyMutex (waitMutex);

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

AudioStream::AudioStream (AudioContext& context, const ReadStreamSharedPtr& buffer) : m_audioContext (context) {
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
    int64_t out_channel_layout;

    switch (this->m_audioContext.getChannels ()) {
	case 1:
	    out_channel_layout = AV_CH_LAYOUT_MONO;
	    break;
	case 2:
	    out_channel_layout = AV_CH_LAYOUT_STEREO;
	    break;
	default:
	    out_channel_layout = AV_CH_LAYOUT_SURROUND;
	    break;
    }

    this->m_swrctx = swr_alloc_set_opts (
	nullptr, out_channel_layout, this->m_audioContext.getFormat (), this->m_audioContext.getSampleRate (),
	this->getContext ()->channel_layout, this->getContext ()->sample_fmt, this->getContext ()->sample_rate, 0,
	nullptr
    );
#else
    AVChannelLayout out_channel_layout;
    int64_t out_channel_mask;

    switch (this->m_audioContext.getChannels ()) {
	case 1:
	    out_channel_mask = AV_CH_LAYOUT_MONO;
	    break;
	case 2:
	    out_channel_mask = AV_CH_LAYOUT_STEREO;
	    break;
	default:
	    out_channel_mask = AV_CH_LAYOUT_SURROUND;
	    break;
    }

    if (av_channel_layout_from_mask (&out_channel_layout, out_channel_mask) != 0) {
	sLog.exception ("Cannot get channel layout from mask");
    }

    swr_alloc_set_opts2 (
	&this->m_swrctx, &out_channel_layout, this->m_audioContext.getFormat (), this->m_audioContext.getSampleRate (),
	&this->m_context->ch_layout, this->m_context->sample_fmt, this->m_context->sample_rate, 0, nullptr
    );
#endif

    if (this->m_swrctx == nullptr) {
	sLog.exception ("Cannot initialize swrctx for audio resampling");
    }

    if (swr_init (this->m_swrctx) < 0) {
	sLog.exception ("Failed to initialize the resampling context.");
    }

    this->m_queue->mutex = SDL_CreateMutex ();
    this->m_queue->cond = SDL_CreateCond ();
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

    SDL_CondSignal (this->m_queue->cond);

    return true;
}

void AudioStream::dequeuePacket () {
    MyAVPacketList entry {};

    SDL_LockMutex (this->m_queue->mutex);

    while (this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
#if FF_API_FIFO_OLD_API
	int ret = -1;

	if (av_fifo_size (this->m_queue->packetList) >= static_cast<int> (sizeof (entry))) {
	    ret = av_fifo_generic_read (this->m_queue->packetList, &entry, sizeof (entry), nullptr);
	}
#else
	const int ret = av_fifo_read (this->m_queue->packetList, &entry, 1);
#endif

	// enough data available, read it
	if (ret >= 0) {
	    this->m_queue->nb_packets--;
	    this->m_queue->size -= entry.packet->size + sizeof (entry);
	    this->m_queue->duration -= entry.packet->duration;

	    av_packet_move_ref (this->m_decodePacket, entry.packet);
	    av_packet_free (&entry.packet);
	    break;
	}

	SDL_CondWait (this->m_queue->cond, this->m_queue->mutex);
    }

    SDL_UnlockMutex (this->m_queue->mutex);
}

AVCodecContext* AudioStream::getContext () const { return this->m_context; }

AVFormatContext* AudioStream::getFormatContext () const { return this->m_formatContext; }

int AudioStream::getAudioStream () const { return this->m_audioStream; }

bool AudioStream::isInitialized () const { return this->m_initialized; }

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

bool AudioStream::isQueueEmpty () const { return this->m_queue->nb_packets == 0; }

SDL_mutex* AudioStream::getMutex () const { return this->m_queue->mutex; }

void AudioStream::stop () {
    if (!this->isInitialized ()) {
	return;
    }

    // stop the threads running
    this->m_initialized = false;
}

int AudioStream::resampleAudio (uint8_t* out_buf, const int out_size) {
    int out_linesize = 0;
    int ret;
    int out_nb_channels;
    int out_nb_samples;
    uint8_t** resampled_data = nullptr;
    int resampled_data_size;

    const int in_nb_samples = this->m_decodeFrame->nb_samples;
    if (in_nb_samples <= 0) {
	sLog.error ("in_nb_samples error.");
	return -1;
    }

    int max_out_nb_samples = out_nb_samples = av_rescale_rnd (
	in_nb_samples, this->m_audioContext.getSampleRate (), this->getContext ()->sample_rate, AV_ROUND_UP
    );

    if (max_out_nb_samples <= 0) {
	sLog.error ("av_rescale_rnd error.");
	return -1;
    }

#if FF_API_OLD_CHANNEL_LAYOUT
    int64_t out_channel_layout;

    switch (this->m_audioContext.getChannels ()) {
	case 1:
	    out_channel_layout = AV_CH_LAYOUT_MONO;
	    break;
	case 2:
	    out_channel_layout = AV_CH_LAYOUT_STEREO;
	    break;
	default:
	    out_channel_layout = AV_CH_LAYOUT_SURROUND;
	    break;
    }

    out_nb_channels = av_get_channel_layout_nb_channels (out_channel_layout);
#else
    // Must be the channel count swr_convert() below will actually write, not the input file's
    // channel count - a mono sound resampled to a stereo driver output would otherwise get a
    // buffer sized for one channel while swr_convert() writes two, overflowing it.
    out_nb_channels = this->m_audioContext.getChannels ();
#endif
    ret = av_samples_alloc_array_and_samples (
	&resampled_data, &out_linesize, out_nb_channels, out_nb_samples, this->m_audioContext.getFormat (), 0
    );

    if (ret < 0) {
	sLog.error ("av_samples_alloc_array_and_samples() error: Could not allocate destination samples.");
	return -1;
    }

    // account for the progressive resampling delay
    out_nb_samples = av_rescale_rnd (
	swr_get_delay (this->m_swrctx, this->getContext ()->sample_rate) + in_nb_samples,
	this->m_audioContext.getSampleRate (), this->getContext ()->sample_rate, AV_ROUND_UP
    );

    if (out_nb_samples <= 0) {
	sLog.error ("av_rescale_rnd error");
	return -1;
    }

    if (out_nb_samples > max_out_nb_samples) {
	av_freep (&resampled_data[0]);

	ret = av_samples_alloc (
	    resampled_data, &out_linesize, out_nb_channels, out_nb_samples, this->m_audioContext.getFormat (), 1
	);

	if (ret < 0) {
	    sLog.error ("av_samples_alloc failed.");
	    return -1;
	}

	max_out_nb_samples = out_nb_samples;
    }

    ret = swr_convert (
	this->m_swrctx, resampled_data, max_out_nb_samples, const_cast<const uint8_t**> (this->m_decodeFrame->data),
	this->m_decodeFrame->nb_samples
    );

    if (ret < 0) {
	sLog.error ("swr_convert_error.");
	return -1;
    }

    resampled_data_size
	= av_samples_get_buffer_size (&out_linesize, out_nb_channels, ret, this->m_audioContext.getFormat (), 1);

    if (resampled_data_size < 0) {
	sLog.error ("av_samples_get_buffer_size error.");
	return -1;
    }

    memcpy (out_buf, resampled_data[0], std::min (resampled_data_size, out_size));

    if (resampled_data) {
	av_freep (&resampled_data[0]);
    }

    av_freep (&resampled_data);

    return resampled_data_size;
}

int AudioStream::decodeFrame (uint8_t* audioBuffer, const int bufferSize) {
    // block until there's any data in the buffers
    while (this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
	while (this->m_audioPacketSize > 0 && this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
	    int got_frame = 0;
	    int ret = avcodec_receive_frame (this->getContext (), this->m_decodeFrame);

	    if (ret == 0) {
		got_frame = 1;
	    }
	    if (ret == AVERROR (EAGAIN)) {
		ret = 0;
	    }
	    if (ret == 0) {
		ret = avcodec_send_packet (this->getContext (), this->m_decodePacket);
	    }
	    if (ret < 0 && ret != AVERROR (EAGAIN)) {
		return -1;
	    }

	    if (this->m_decodePacket->size < 0) {
		// if error, skip frame
		this->m_audioPacketSize = 0;
		break;
	    }

	    this->m_audioPacketSize -= this->m_decodePacket->size;
	    int data_size = 0;

	    if (got_frame) {
		data_size = this->resampleAudio (audioBuffer, bufferSize);
	    }
	    if (data_size <= 0) {
		// no data found, keep waiting
		continue;
	    }
	    return data_size;
	}

	if (this->m_decodePacket->data) {
	    av_packet_unref (this->m_decodePacket);
	}

	this->dequeuePacket ();

	this->m_audioPacketSize = this->m_decodePacket->size;
    }

    return 0;
}

AudioContext& AudioStream::getAudioContext () const { return this->m_audioContext; }