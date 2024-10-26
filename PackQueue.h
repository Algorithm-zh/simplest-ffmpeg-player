#ifndef PACKETQUEUE_H
#define PACKETQUEUE_H

#include <atomic>
#include <list>

#ifdef __cplusplus
extern "C" {
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
}
#endif
// PacketQueue 的类，用于管理一个 AVPacket（来自 FFmpeg 库）的队列。
// 这个类主要用于在多线程环境中安全地管理和操作数据包队列。
class PacketQueue {
public:
  PacketQueue();

  int packetPut(AVPacket *pkt);

  int packetGet(AVPacket *pkt, std::atomic<bool> &quit);

  void packetFlush();

  int packetSize();

private:
  std::list<AVPacket> pkts;
  // data大小
  std::atomic<int> size = 0;
  // SDL提供的互斥锁和条件变量
  SDL_mutex *mutex = nullptr;
  SDL_cond *cond = nullptr;
};

#endif // PACKETQUEUE_H
