#pragma once

#include "cores/AudioEngine/Interfaces/AESink.h"
#include "threads/Thread.h" // Corrected include path
#include "cores/AudioEngine/Utils/AEDeviceInfo.h" // Added for AEDeviceInfoList
#include "threads/CriticalSection.h"
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

class CAESinkRTPPipeWire : public IAESink, public CThread
{
private:
    // Maximum RTP packet size to prevent buffer overruns (12072 bytes from logs - 12 bytes RTP header)
    static constexpr size_t MAX_RTP_PAYLOAD_SIZE = 1150;
    static constexpr size_t MAX_SAMPLES_PER_PACKET = 1024;  // Will be adjusted based on format
    
public:
  static void Register(); // Added static Register method declaration
  static std::unique_ptr<IAESink> Create(std::string& device, AEAudioFormat& desiredFormat);
  static void EnumerateDevicesEx(AEDeviceInfoList& list, bool force);
  CAESinkRTPPipeWire() noexcept; // Added noexcept
  ~CAESinkRTPPipeWire() override;

  // IAESink methods
  bool Initialize(AEAudioFormat& format, std::string& device) override; // Corrected signature
  unsigned int AddPackets(uint8_t** data, unsigned int frames, unsigned int offset) override; // Corrected signature
  void FreeSpace(int& free); // Removed override
  void GetDelay(AEDelayStatus& status) override; // Corrected signature
  double GetLatency() override; // Kept override, will return 0.0 in cpp
  void Stop(); // Removed override
  void Pause(); // Removed override
  void Resume(); // Removed override
  void WaitUntilWeCanWrite(); // Removed override
  void ClearBuffers(); // Removed override
  void Flush(); // Removed override
  void Drain() override; // Added override back
  void Deinitialize() override;
  const char* GetName() override; // Declared GetName()
  double GetCacheTotal() override; // Declared GetCacheTotal()

protected:
  // CThread methods
  void Process() override;

private:
  void SwapBytes(uint8_t* data, unsigned int size);
  bool InitFFmpeg(const AEAudioFormat& format); // Corrected signature
  void CloseFFmpeg();

  CCriticalSection m_critSection;
  std::string m_remoteIp;
  int m_remotePort;

  AEAudioFormat m_format;

  AVFormatContext* m_formatContext;
  AVCodecContext* m_codecContext;
  AVStream* m_audioStream;
  AVPacket* m_packet;
  AVFrame* m_frame;
  SwrContext* m_swrContext;

  int64_t m_pts;
  int64_t m_firstPts;  // First PTS for timing reference
  int m_samplesCount;
  bool m_started;
  std::chrono::steady_clock::time_point m_startTime;
  
  // Rate emulation (FFmpeg -re equivalent)
  bool m_rateEmu;
  unsigned int m_channelCount;
  std::vector<std::vector<uint8_t>> m_planar_buffer;
  std::vector<uint8_t> m_interleaved_buffer;
};