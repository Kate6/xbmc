#include "AESinkRTPPipeWire.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h" // Explicitly include AEDeviceInfo.h
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/AESinkFactory.h"
#include <mutex>
#include "utils/log.h"
#include "settings/SettingsComponent.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include <algorithm>
#include <vector>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// Anonymous namespace for utility functions
namespace {
} // Anonymous namespace for utility functions

CAESinkRTPPipeWire::CAESinkRTPPipeWire() noexcept // Added noexcept and CThread base class init
    : CThread("AESinkRTPPipeWire")
    , m_formatContext(nullptr)
    , m_codecContext(nullptr)
    , m_audioStream(nullptr)
    , m_packet(nullptr)
    , m_frame(nullptr)
    , m_swrContext(nullptr)
    , m_pts(0)
    , m_firstPts(AV_NOPTS_VALUE)
    , m_samplesCount(0)
    , m_started(false)
    , m_rateEmu(true)
{
}

CAESinkRTPPipeWire::~CAESinkRTPPipeWire()
{
  Deinitialize();
}

bool CAESinkRTPPipeWire::Initialize(AEAudioFormat& format, std::string& device)
{
  m_remoteIp = CServiceBroker::GetSettingsComponent()->GetSettings()->GetString("audiooutput.pipewire.host");
  m_remotePort = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("audiooutput.pipewire.port");

  // If the sink is already running, this indicates a format change.
  // Stop the thread and close FFmpeg resources before re-initializing.
  if (m_started)
  {
    CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Re-initializing for new audio format.");
    Stop();
  }

  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Initializing...");

  CLog::Log(LOGDEBUG,
            "CAESinkRTPPipeWire: Initialize with sampleRate={}, channels={}, dataFormat={}, device='{}'",
            format.m_sampleRate,
            format.m_channelLayout.Count(),
            static_cast<int>(format.m_dataFormat),
            device);

  // Store format for later use in AddPackets and other methods
  m_format = format;

  if (!InitFFmpeg(format))
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Failed to initialize FFmpeg.");
    CloseFFmpeg(); // Clean up any partially allocated resources
    m_started = false; // Ensure we know we're not started
    return false;
  }

  unsigned int channels = format.m_channelLayout.Count();
  unsigned int bitsPerSample = CAEUtil::DataFormatToBits(format.m_dataFormat);
  if (channels == 0 || bitsPerSample == 0 || format.m_sampleRate == 0)
  {
    CLog::Log(LOGERROR,
              "CAESinkRTPPipeWire: Invalid audio format (channels={}, bitsPerSample={}, sampleRate={}), using safe defaults",
              channels,
              bitsPerSample,
              format.m_sampleRate);
    format.m_dataFormat = AE_FMT_FLOAT;
    format.m_sampleRate = 44100;
    format.m_channelLayout = CAEChannelInfo(AE_CH_LAYOUT_2_0);
    channels = format.m_channelLayout.Count();
    bitsPerSample = CAEUtil::DataFormatToBits(format.m_dataFormat);
  }

  format.m_frameSize = (bitsPerSample / 8) * channels;

  if (format.m_frames == 0)
  {
    format.m_frames = std::max(1u, format.m_sampleRate / 10u);
  }

  m_format = format;
  m_channelCount = format.m_channelLayout.Count();
  if (AE_IS_PLANAR(m_format.m_dataFormat))
  {
    m_planar_buffer.resize(m_channelCount);
  }
  ClearBuffers();

  // Initialize timing state so we can pace RTP output in (approximate) real time.
  m_pts = 0;
  m_firstPts = AV_NOPTS_VALUE;
  m_samplesCount = 0;
  m_startTime = std::chrono::steady_clock::now();
  m_started = true; // Set started flag *after* successful initialization

  // Start the processing thread
  CThread::Create(false);
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Initialized successfully.");
  return true;
}

bool CAESinkRTPPipeWire::InitFFmpeg(const AEAudioFormat& format)
{
  std::unique_lock<CCriticalSection> lock(m_critSection);
  int ret;

  // 1. Find the encoder for PCM S16LE
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
  if (!codec)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Codec 'pcm_s16le' not found.");
    return false;
  }

  m_codecContext = avcodec_alloc_context3(codec);
  if (!m_codecContext)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate audio codec context.");
    return false;
  }

  // 2. Configure the codec context to match the ffmpeg command
  m_codecContext->sample_fmt = AV_SAMPLE_FMT_S16;
  m_codecContext->sample_rate = 48000;
  av_channel_layout_default(&m_codecContext->ch_layout, 2); // 2 channels for stereo
  m_codecContext->time_base = {1, m_codecContext->sample_rate};

  CLog::Log(LOGDEBUG,
            "CAESinkRTPPipeWire: Codec context configured: sample_fmt={}, sample_rate={}, channels={}",
            av_get_sample_fmt_name(m_codecContext->sample_fmt),
            m_codecContext->sample_rate,
            m_codecContext->ch_layout.nb_channels);

  ret = avcodec_open2(m_codecContext, codec, nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not open codec: {}", av_err2str(ret));
    return false;
  }

  // 3. Allocate output format context for RTP
  ret = avformat_alloc_output_context2(&m_formatContext, nullptr, "rtp", nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate RTP output context: {}", av_err2str(ret));
    return false;
  }

  // 4. Set the RTP URL with pkt_size parameter
  char rtp_url[256];
  snprintf(rtp_url, sizeof(rtp_url), "rtp://%s:%d?pkt_size=1200", m_remoteIp.c_str(), m_remotePort);
  m_formatContext->url = av_strdup(rtp_url);
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: RTP output URL set to {}", m_formatContext->url);

  // 5. Add the audio stream and copy codec parameters
  m_audioStream = avformat_new_stream(m_formatContext, nullptr);
  if (!m_audioStream)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate audio stream.");
    return false;
  }
  m_audioStream->time_base = m_codecContext->time_base;
  ret = avcodec_parameters_from_context(m_audioStream->codecpar, m_codecContext);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not copy stream parameters: {}", av_err2str(ret));
    return false;
  }

  // 6. Open the RTP output for writing
  ret = avio_open(&m_formatContext->pb, m_formatContext->url, AVIO_FLAG_WRITE);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not open RTP output: {}", av_err2str(ret));
    return false;
  }

  // 7. Write the stream header. No special options needed.
  ret = avformat_write_header(m_formatContext, nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Error writing stream header: {}", av_err2str(ret));
    return false;
  }

  // 8. Allocate reusable frame and packet
  m_frame = av_frame_alloc();
  if (!m_frame)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate audio frame.");
    return false;
  }
  m_frame->format = m_codecContext->sample_fmt;
  av_channel_layout_copy(&m_frame->ch_layout, &m_codecContext->ch_layout);
  m_frame->sample_rate = m_codecContext->sample_rate;
  m_frame->nb_samples = 1024;

  ret = av_frame_get_buffer(m_frame, 0);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate frame samples: {}", av_err2str(ret));
    return false;
  }

  m_packet = av_packet_alloc();
  if (!m_packet)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate AVPacket.");
    return false;
  }

  // 9. Initialize SwrContext for resampling and format conversion
  AVChannelLayout in_layout;
  av_channel_layout_from_mask(&in_layout, CAEUtil::GetAVChannelLayout(format.m_channelLayout));
  swr_alloc_set_opts2(&m_swrContext,
                      &m_codecContext->ch_layout, (AVSampleFormat)m_codecContext->sample_fmt, m_codecContext->sample_rate,
                      &in_layout, CAEUtil::GetAVSampleFormat(format.m_dataFormat), format.m_sampleRate,
                      0, nullptr);
  if (!m_swrContext)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not allocate SwrContext.");
    return false;
  }

  ret = swr_init(m_swrContext);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Could not initialize SwrContext: {}", av_err2str(ret));
    return false;
  }

  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: FFmpeg initialized for RTP output to %s:%d", m_remoteIp.c_str(), m_remotePort);
  return true;
}

void CAESinkRTPPipeWire::CloseFFmpeg()
{
  std::unique_lock<CCriticalSection> lock(m_critSection);
  if (m_formatContext)
  {
    if (m_formatContext->pb)
    {
      CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Closing RTP output");
      // Only write trailer if the header was successfully written
      if (m_started && (m_formatContext->oformat->flags & AVFMT_NOFILE) == 0) {
        CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Writing trailer");
        av_write_trailer(m_formatContext);
      }
      CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Closing I/O context");
      avio_closep(&m_formatContext->pb);
      m_formatContext->pb = nullptr;
    }
    CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Freeing format context");
    avformat_free_context(m_formatContext);
    m_formatContext = nullptr;
  }
  if (m_codecContext)
  {
    CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Freeing codec context");
    avcodec_free_context(&m_codecContext);
    m_codecContext = nullptr;
  }
  if (m_frame)
  {
    CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Freeing frame");
    av_frame_free(&m_frame);
    m_frame = nullptr;
  }
  if (m_packet)
  {
    CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Freeing packet");
    av_packet_free(&m_packet);
    m_packet = nullptr;
  }
  if (m_swrContext)
  {
    CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Freeing SwrContext");
    swr_free(&m_swrContext);
    m_swrContext = nullptr;
  }
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: FFmpeg resources closed.");
}



unsigned int CAESinkRTPPipeWire::AddPackets(uint8_t** data, unsigned int frames, unsigned int offset)
{
  std::unique_lock<CCriticalSection> lock(m_critSection);

  if (!data || !data[0] || frames == 0)
    return 0;

  if (!m_formatContext || !m_codecContext || !m_swrContext || !m_frame || !m_packet)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: AddPackets called but FFmpeg is not properly initialized");
    return 0;
  }

  const bool isPlanar = AE_IS_PLANAR(m_format.m_dataFormat);
  const unsigned int bytesPerSamplePerChannel = m_format.m_frameSize / m_channelCount;

  // Append incoming data to the correct buffer
  if (isPlanar)
  {
    for (unsigned int ch = 0; ch < m_channelCount; ++ch)
    {
      const size_t bytesToAdd = static_cast<size_t>(frames) * bytesPerSamplePerChannel;
      const uint8_t* start = data[ch] + offset * bytesPerSamplePerChannel;
      m_planar_buffer[ch].insert(m_planar_buffer[ch].end(), start, start + bytesToAdd);
    }
  }
  else
  {
    const size_t bytesToAdd = static_cast<size_t>(frames) * m_format.m_frameSize;
    const uint8_t* start = data[0] + offset * m_format.m_frameSize;
    m_interleaved_buffer.insert(m_interleaved_buffer.end(), start, start + bytesToAdd);
  }

  // Use the allocated frame's sample count as the amount to process
  const int outFrames = m_frame->nb_samples;
  if (outFrames <= 0)
  {
    CLog::Log(LOGERROR, "CAESinkRTPPipeWire: Invalid frame sample count");
    return 0;
  }
  
  const int inFrames = av_rescale_rnd(outFrames, m_format.m_sampleRate, m_codecContext->sample_rate, AV_ROUND_UP);

  while (true)
  {
    size_t requiredBufferSize;
    if (isPlanar)
    {
      requiredBufferSize = static_cast<size_t>(inFrames) * bytesPerSamplePerChannel;
      if (m_planar_buffer.empty() || m_planar_buffer[0].size() < requiredBufferSize)
        break;
    }
    else
    {
      requiredBufferSize = static_cast<size_t>(inFrames) * m_format.m_frameSize;
      if (m_interleaved_buffer.size() < requiredBufferSize)
        break;
    }

    const uint8_t* inData[AV_NUM_DATA_POINTERS] = { nullptr };
    if (isPlanar)
    {
      for (unsigned int ch = 0; ch < m_channelCount; ++ch)
      {
        inData[ch] = m_planar_buffer[ch].data();
      }
    }
    else
    {
      inData[0] = m_interleaved_buffer.data();
    }
    
    // The output from swr_convert is interleaved and goes into m_frame
    int outSamples = swr_convert(m_swrContext, m_frame->data, outFrames, inData, inFrames);

    if (outSamples < 0)
    {
      CLog::Log(LOGERROR, "CAESinkRTPPipeWire: swr_convert failed: {}", av_err2str(outSamples));
      ClearBuffers();
      return 0;
    }

    m_frame->nb_samples = outSamples;

    if (m_frame->nb_samples > 0)
    {
      m_frame->pts = m_pts;
      m_pts += m_frame->nb_samples;
      int ret = avcodec_send_frame(m_codecContext, m_frame);
      if (ret < 0)
      {
        CLog::Log(LOGERROR, "CAESinkRTPPipeWire: avcodec_send_frame failed: {}", av_err2str(ret));
        break;
      }

      while (ret >= 0)
      {
        ret = avcodec_receive_packet(m_codecContext, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        if (ret < 0)
        {
          CLog::Log(LOGERROR, "CAESinkRTPPipeWire: avcodec_receive_packet failed: {}", av_err2str(ret));
          break;
        }

        // Rate emulation similar to FFmpeg's -re option
        if (m_rateEmu && m_codecContext->sample_rate > 0)
        {
          const auto now = std::chrono::steady_clock::now();
          if (m_firstPts == AV_NOPTS_VALUE)
          {
            m_firstPts = m_packet->pts;
            m_startTime = now;
          }

          const int64_t elapsedPts = m_packet->pts - m_firstPts;
          const auto targetElapsed = std::chrono::microseconds(static_cast<int64_t>(
              (elapsedPts * 1000000.0) / m_codecContext->sample_rate));
          const auto targetTime = m_startTime + targetElapsed;

          if (targetTime > now)
          {
            lock.unlock();
            std::this_thread::sleep_for(targetTime - now);
            lock.lock();
          }
        }
        
        m_packet->stream_index = m_audioStream->index;
        av_packet_rescale_ts(m_packet, m_codecContext->time_base, m_audioStream->time_base);
        
        // Actually send the packet
        ret = av_interleaved_write_frame(m_formatContext, m_packet);
        if (ret < 0)
        {
            CLog::Log(LOGERROR, "CAESinkRTPPipeWire: av_interleaved_write_frame failed: {}", av_err2str(ret));
        }

        av_packet_unref(m_packet);
      }
    }

    // Erase the consumed data from the input buffers
    if (isPlanar)
    {
      const size_t bytesConsumed = static_cast<size_t>(inFrames) * bytesPerSamplePerChannel;
      for (unsigned int ch = 0; ch < m_channelCount; ++ch)
      {
        m_planar_buffer[ch].erase(m_planar_buffer[ch].begin(), m_planar_buffer[ch].begin() + bytesConsumed);
      }
    }
    else
    {
      const size_t bytesConsumed = static_cast<size_t>(inFrames) * m_format.m_frameSize;
      m_interleaved_buffer.erase(m_interleaved_buffer.begin(), m_interleaved_buffer.begin() + bytesConsumed);
    }
  }

  return frames;
}

void CAESinkRTPPipeWire::FreeSpace(int& free)
{
  // Placeholder: In a real implementation, this would reflect the available buffer space.
  // For now, assume always ready to receive.
  free = 1024 * 1024; // A large arbitrary number
}

void CAESinkRTPPipeWire::GetDelay(AEDelayStatus& status)
{
  status.SetDelay(GetLatency());
}

double CAESinkRTPPipeWire::GetLatency()
{
  std::unique_lock<CCriticalSection> lock(m_critSection);

  if (!m_started || !m_codecContext || m_codecContext->sample_rate <= 0)
    return 0.0;

  const double playedDurationSecs = static_cast<double>(m_samplesCount) / m_codecContext->sample_rate;
  const auto now = std::chrono::steady_clock::now();
  const auto elapsedSecs = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_startTime).count();

  double senderBufferDelay = playedDurationSecs - elapsedSecs;
  if (senderBufferDelay < 0.0)
    senderBufferDelay = 0.0;

  // Add a small, fixed estimate for network latency and receiver jitter buffer.
  constexpr double kEstimatedRemoteLatencySeconds = 0.05; // 50ms

  return senderBufferDelay + kEstimatedRemoteLatencySeconds;
}

void CAESinkRTPPipeWire::Stop()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Stopping.");
  StopThread(); // Corrected call
  CloseFFmpeg();
  m_started = false;
}

void CAESinkRTPPipeWire::SwapBytes(uint8_t* data, unsigned int size)
{
  for (unsigned int i = 0; i < size; i += 2)
  {
    std::swap(data[i], data[i + 1]);
  }
}

void CAESinkRTPPipeWire::Pause()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Paused.");
  // No specific action for RTP sending on pause, packets will just stop flowing.
}

void CAESinkRTPPipeWire::Resume()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Resumed.");
  // Reset the rate emulation clock to avoid trying to "catch up" after a pause.
  if (m_rateEmu)
  {
    m_firstPts = AV_NOPTS_VALUE;
  }
}

void CAESinkRTPPipeWire::WaitUntilWeCanWrite()
{
  // Placeholder: In a real implementation, this would block if buffers are full.
  // For now, assume non-blocking.
}

void CAESinkRTPPipeWire::ClearBuffers()
{
  std::unique_lock<CCriticalSection> lock(m_critSection);
  
  if (m_codecContext)
  {
    avcodec_flush_buffers(m_codecContext);
  }
  
  m_interleaved_buffer.clear();
  for (auto& buf : m_planar_buffer)
  {
    buf.clear();
  }
  
  m_pts = 0;
  m_samplesCount = 0;
  m_firstPts = AV_NOPTS_VALUE;
  m_startTime = std::chrono::steady_clock::now();
  
  if (m_rateEmu) {
    m_firstPts = AV_NOPTS_VALUE;
  }
}

void CAESinkRTPPipeWire::Flush()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Flushing.");
  ClearBuffers();
}

void CAESinkRTPPipeWire::Drain()
{
  std::unique_lock<CCriticalSection> lock(m_critSection);
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Draining.");

  if (!m_interleaved_buffer.empty())
  {
    CLog::Log(LOGWARNING, "CAESinkRTPPipeWire: Discarding {} bytes from interleaved buffer during drain.", m_interleaved_buffer.size());
  }
  for (size_t i = 0; i < m_planar_buffer.size(); ++i)
  {
    if (!m_planar_buffer[i].empty())
    {
      CLog::Log(LOGWARNING, "CAESinkRTPPipeWire: Discarding {} bytes from planar buffer channel {} during drain.", m_planar_buffer[i].size(), i);
    }
  }

  // Send null frames to drain the encoder
  int ret;
  if (m_codecContext)
  {
    ret = avcodec_send_frame(m_codecContext, nullptr); // Drain
    if (ret >= 0)
    {
      while (true)
      {
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
    }
  }
  ClearBuffers();
}

void CAESinkRTPPipeWire::Deinitialize()
{
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Deinitializing.");
  Stop(); // Ensure thread is stopped and FFmpeg resources are closed
}

const char* CAESinkRTPPipeWire::GetName()
{
  return "RTPPipeWire";
}

double CAESinkRTPPipeWire::GetCacheTotal()
{
  return 0.0; // Placeholder for now
}

void CAESinkRTPPipeWire::Process()
{
#if defined(TARGET_ANDROID)
  SetPriority(ThreadPriority::HIGHEST);
#endif
  CLog::Log(LOGDEBUG, "CAESinkRTPPipeWire: Processing thread started.");
  while (!m_bStop) // Corrected usage
  {
    Sleep(std::chrono::milliseconds(10)); // Corrected usage
  }
  CLog::Log(LOGINFO, "CAESinkRTPPipeWire: Processing thread stopped.");
}

// Added Register, Create, EnumerateDevicesEx implementations
void CAESinkRTPPipeWire::Register()
{
  AE::AESinkRegEntry entry;
  entry.sinkName = "RTPPipeWire";
  entry.createFunc = CAESinkRTPPipeWire::Create;
  entry.enumerateFunc = CAESinkRTPPipeWire::EnumerateDevicesEx;
  AE::CAESinkFactory::RegisterSink(entry);
}

std::unique_ptr<IAESink> CAESinkRTPPipeWire::Create(std::string& device,
                                                   AEAudioFormat& desiredFormat)
{
  auto sink = std::make_unique<CAESinkRTPPipeWire>();
  if (sink->Initialize(desiredFormat, device))
    return sink;

  return {};
}

void CAESinkRTPPipeWire::EnumerateDevicesEx(AEDeviceInfoList& list, bool force)
{
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool("audiooutput.pipewire.enabled"))
    return;

  // Placeholder implementation for now.
  // In a real scenario, this would query PipeWire for available RTP sinks.
  CAEDeviceInfo info;
  info.m_deviceName = "PipeWire RTP Sink";
  info.m_displayName = "PipeWire RTP";
  info.m_deviceType = AE_DEVTYPE_PCM; // Assuming PCM for now
  info.m_dataFormats.push_back(AE_FMT_S16LE); // Example format
  info.m_sampleRates.push_back(48000); // Example sample rate
  info.m_channels = CAEChannelInfo(AE_CH_LAYOUT_2_0); // Example channel layout
  list.push_back(info);
}
