#pragma once

#include "cores/AudioEngine/Interfaces/AESink.h"
#include "utils/Thread.h"
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

class CAESinkRTPPipeWire : public IAESink, public CThread
{
public:
  CAESinkRTPPipeWire();
  ~CAESinkRTPPipeWire() override;

  // IAESink methods
  bool Initialize(const AE_SETTINGS& aesettings, const AE_STREAM_INFO& info) override;
  bool AddPackets(const ActiveAE::FrameList& packets) override;
  void FreeSpace(int& free) override;
  double GetDelay() override;
  double GetLatency() override;
  void Stop() override;
  void Pause() override;
  void Resume() override;
  void WaitUntilWeCanWrite() override;
  void ClearBuffers() override;
  void Flush() override;
  void Drain() override;
  void Deinitialize() override;

protected:
  // CThread methods
  void Process() override;

private:
  bool InitFFmpeg(const AE_STREAM_INFO& info);
  void CloseFFmpeg();

  std::string m_remoteIp;
  int m_remotePort;

  AVFormatContext* m_formatContext;
  AVCodecContext* m_codecContext;
  AVStream* m_audioStream;
  AVPacket* m_packet;
  AVFrame* m_frame;
  SwrContext* m_swrContext;

  int64_t m_pts;
  int m_samplesCount;
};
