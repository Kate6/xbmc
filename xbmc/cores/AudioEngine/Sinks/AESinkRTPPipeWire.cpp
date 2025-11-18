#include "AESinkRTPPipeWire.h"
#include "cores/AudioEngine/AudioEngine.h"
#include "utils/log.h"
#include "settings/AdvancedSettings.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// Anonymous namespace for utility functions
namespace {
  AVCodecID get_av_codec_id(const AE_STREAM_INFO& info)
  {
    // This is a placeholder. In a real implementation, you'd map Kodi's audio
    // format to an appropriate FFmpeg codec, potentially based on user settings
    // or negotiation with the PipeWire daemon. For now, we'll assume Opus.
    return AV_CODEC_ID_OPUS;
  }

  AVSampleFormat get_av_sample_format(const AE_STREAM_INFO& info)
  {
    // Placeholder: Map Kodi's sample format to FFmpeg's.
    // For Opus, float or S16 is common.
    return AV_SAMPLE_FMT_FLTP; // Float planar
  }

  int get_av_channel_layout(const AE_STREAM_INFO& info)
  {
    // Placeholder: Map Kodi's channel layout to FFmpeg's.
    // For stereo, AV_CH_LAYOUT_STEREO.
    return AV_CH_LAYOUT_STEREO;
  }
}

CAESinkRTPPipeWire::CAESinkRTPPipeWire()
    : m_formatContext(nullptr)
    , m_codecContext(nullptr)
    , m_audioStream(nullptr)
    , m_packet(nullptr)
    , m_frame(nullptr)
    , m_swrContext(nullptr)
    , m_pts(0)
    , m_samplesCount(0)
{
  // Default remote IP and port (hardcoded for now)
  m_remoteIp = "127.0.0.1"; // Loopback for testing
  m_remotePort = 5004;     // Default RTP port
}

CAESinkRTPPipeWire::~CAESinkRTPPipeWire()
{
  Deinitialize();
}

bool CAESinkRTPPipeWire::Initialize(const AE_SETTINGS& aesettings, const AE_STREAM_INFO& info)
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Initializing...");

  if (!InitFFmpeg(info))
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Failed to initialize FFmpeg.");
    return false;
  }

  // Start the processing thread
  Create(true); // true for auto-delete
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Initialized successfully.");
  return true;
}

bool CAESinkRTPPipeWire::InitFFmpeg(const AE_STREAM_INFO& info)
{
  int ret;

  // Find the encoder
  AVCodecID codec_id = get_av_codec_id(info);
  const AVCodec* codec = avcodec_find_encoder(codec_id);
  if (!codec)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Codec not found: %d", codec_id);
    return false;
  }

  m_codecContext = avcodec_alloc_context3(codec);
  if (!m_codecContext)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate audio codec context.");
    return false;
  }

  m_codecContext->sample_fmt = get_av_sample_format(info);
  m_codecContext->bit_rate = 64000; // Example bitrate for Opus
  m_codecContext->sample_rate = info.m_sampleRate;
  m_codecContext->channel_layout = get_av_channel_layout(info);
  m_codecContext->channels = info.m_channels;

  // Some codecs require a global header
  if (m_formatContext && m_formatContext->oformat && (m_formatContext->oformat->flags & AVFMT_GLOBALHEADER))
    m_codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  ret = avcodec_open2(m_codecContext, codec, nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not open codec: %s", av_err2str(ret));
    return false;
  }

  // Allocate output format context for RTP
  ret = avformat_alloc_output_context2(&m_formatContext, nullptr, "rtp", nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate output format context for RTP: %s", av_err2str(ret));
    return false;
  }

  // Set the remote address for RTP
  char sdp_file[256];
  snprintf(sdp_file, sizeof(sdp_file), "rtp://%s:%d", m_remoteIp.c_str(), m_remotePort);
  m_formatContext->url = av_strdup(sdp_file);

  // Add the audio stream
  m_audioStream = avformat_new_stream(m_formatContext, codec);
  if (!m_audioStream)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate audio stream.");
    return false;
  }

  ret = avcodec_parameters_from_context(m_audioStream->codecpar, m_codecContext);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not copy the stream parameters: %s", av_err2str(ret));
    return false;
  }

  // Open the RTP output
  ret = avio_open(&m_formatContext->pb, m_formatContext->url, AVIO_FLAG_WRITE);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not open RTP output: %s", av_err2str(ret));
    return false;
  }

  // Write the stream header
  ret = avformat_write_header(m_formatContext, nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Error writing stream header: %s", av_err2str(ret));
    return false;
  }

  // Allocate frame and packet
  m_frame = av_frame_alloc();
  if (!m_frame)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate audio frame.");
    return false;
  }
  m_frame->nb_samples = m_codecContext->frame_size;
  m_frame->format = m_codecContext->sample_fmt;
  m_frame->channel_layout = m_codecContext->channel_layout;
  m_frame->sample_rate = m_codecContext->sample_rate;

  ret = av_frame_get_buffer(m_frame, 0);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate audio frame samples: %s", av_err2str(ret));
    return false;
  }

  m_packet = av_packet_alloc();
  if (!m_packet)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate AVPacket.");
    return false;
  }

  // Initialize SwrContext for resampling/format conversion
  m_swrContext = swr_alloc_set_opts(nullptr,
                                    m_codecContext->channel_layout,
                                    m_codecContext->sample_fmt,
                                    m_codecContext->sample_rate,
                                    av_get_default_channel_layout(info.m_channels), // Input channel layout
                                    (AVSampleFormat)info.m_sampleFormat,             // Input sample format
                                    info.m_sampleRate,                               // Input sample rate
                                    0, nullptr);
  if (!m_swrContext)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate SwrContext.");
    return false;
  }

  ret = swr_init(m_swrContext);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not initialize SwrContext: %s", av_err2str(ret));
    swr_free(&m_swrContext);
    return false;
  }

  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: FFmpeg initialized for RTP output to %s:%d", m_remoteIp.c_str(), m_remotePort);
  return true;
}

void CAESinkRTPPipeWire::CloseFFmpeg()
{
  if (m_formatContext)
  {
    if (m_formatContext->pb)
    {
      av_write_trailer(m_formatContext);
      avio_closep(&m_formatContext->pb);
    }
    avformat_free_context(m_formatContext);
    m_formatContext = nullptr;
  }
  if (m_codecContext)
  {
    avcodec_free_context(&m_codecContext);
    m_codecContext = nullptr;
  }
  if (m_frame)
  {
    av_frame_free(&m_frame);
    m_frame = nullptr;
  }
  if (m_packet)
  {
    av_packet_free(&m_packet);
    m_packet = nullptr;
  }
  if (m_swrContext)
  {
    swr_free(&m_swrContext);
    m_swrContext = nullptr;
  }
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: FFmpeg resources closed.");
}

bool CAESinkRTPPipeWire::AddPackets(const ActiveAE::FrameList& packets)
{
  // This method will be called by Kodi's audio engine with raw audio data.
  // We need to encode it and send it via RTP.

  for (const auto& packet : packets)
  {
    // Convert incoming audio data using swr_convert
    const uint8_t* input_data[AV_NUM_DATA_POINTERS];
    input_data[0] = (const uint8_t*)packet->data;

    int converted_samples = swr_convert(m_swrContext,
                                        m_frame->data,
                                        m_frame->nb_samples,
                                        input_data,
                                        packet->size / (av_get_bytes_per_sample((AVSampleFormat)packet->format) * packet->channels));

    if (converted_samples < 0)
    {
      CLog::Log(LOGERROR, "CAESinkRTPPipeWire: swr_convert failed: %s", av_err2str(converted_samples));
      return false;
    }

    if (converted_samples > 0)
    {
      m_frame->pts = m_pts;
      m_samplesCount += converted_samples;
      m_pts = av_rescale_q(m_samplesCount, {1, m_codecContext->sample_rate}, m_audioStream->time_base);

      int ret = avcodec_send_frame(m_codecContext, m_frame);
      if (ret < 0)
      {
        CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Error sending a frame for encoding: %s", av_err2str(ret));
        return false;
      }

      while (ret >= 0)
      {
        ret = avcodec_receive_packet(m_codecContext, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        else if (ret < 0)
        {
          CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Error during encoding: %s", av_err2str(ret));
          return false;
        }

        // Set packet stream index and rescale timestamp
        m_packet->stream_index = m_audioStream->index;
        av_packet_rescale_ts(m_packet, m_codecContext->time_base, m_audioStream->time_base);

        // Write the encoded packet to the RTP output
        ret = av_interleaved_write_frame(m_formatContext, m_packet);
        if (ret < 0)
        {
          CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Error writing audio packet: %s", av_err2str(ret));
          return false;
        }
        av_packet_unref(m_packet);
      }
    }
  }
  return true;
}

void CAESinkRTPPipeWire::FreeSpace(int& free)
{
  // Placeholder: In a real implementation, this would reflect the available buffer space.
  // For now, assume always ready to receive.
  free = 1024 * 1024; // A large arbitrary number
}

double CAESinkRTPPipeWire::GetDelay()
{
  // Placeholder: Calculate actual delay based on buffering and network.
  return 0.0;
}

double CAESinkRTPPipeWire::GetLatency()
{
  // Placeholder: Return estimated latency.
  // This should ideally be configurable and dynamically adjusted.
  return (double)g_advancedSettings.m_audioSinkLatency / 1000.0; // From advanced settings
}

void CAESinkRTPPipeWire::Stop()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Stopping.");
  RequestStop();
  CloseFFmpeg();
}

void CAESinkRTPPipeWire::Pause()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Paused.");
  // No specific action for RTP sending on pause, packets will just stop flowing.
}

void CAESinkRTPPipeWire::Resume()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Resumed.");
  // No specific action for RTP sending on resume.
}

void CAESinkRTPPipeWire::WaitUntilWeCanWrite()
{
  // Placeholder: In a real implementation, this would block if buffers are full.
  // For now, assume non-blocking.
}

void CAESinkRTPPipeWire::ClearBuffers()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Clearing buffers.");
  // Reset PTS and samples count
  m_pts = 0;
  m_samplesCount = 0;
  // Flush encoder
  if (m_codecContext)
  {
    avcodec_send_frame(m_codecContext, nullptr); // Flush
    av_packet_unref(m_packet); // Clear any pending packets
  }
}

void CAESinkRTPPipeWire::Flush()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Flushing.");
  ClearBuffers();
}

void CAESinkRTPPipeWire::Drain()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Draining.");
  // Send null frames to drain the encoder
  int ret;
  while (true)
  {
    ret = avcodec_send_frame(m_codecContext, nullptr); // Drain
    if (ret < 0)
      break;

    ret = avcodec_receive_packet(m_codecContext, m_packet);
    if (ret == AVERROR_EOF)
      break;
    else if (ret < 0)
    {
      CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Error during draining: %s", av_err2str(ret));
      break;
    }

    m_packet->stream_index = m_audioStream->index;
    av_packet_rescale_ts(m_packet, m_codecContext->time_base, m_audioStream->time_base);
    av_interleaved_write_frame(m_formatContext, m_packet);
    av_packet_unref(m_packet);
  }
  ClearBuffers();
}

void CAESinkRTPPipeWire::Deinitialize()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Deinitializing.");
  Stop(); // Ensure thread is stopped and FFmpeg resources are closed
  CThread::StopThread();
}

void CAESinkRTPPipeWire::Process()
{
  // The main processing loop for the sink.
  // In this RTP sink, most of the work (encoding and sending) happens in AddPackets.
  // This thread could be used for more complex buffering, error handling, or
  // asynchronous sending if needed. For now, it just keeps the thread alive.
  while (!IsStopping())
  {
    Sleep(10); // Sleep for a short period
  }
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Processing thread stopped.");
}
