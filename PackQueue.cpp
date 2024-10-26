#include "PackQueue.h"
#include "SDL_mutex.h"
#include "packet.h"

PacketQueue::PacketQueue() {
  mutex = SDL_CreateMutex();
  cond = SDL_CreateCond();
}

int PacketQueue::packetPut(AVPacket *pkt) {
  SDL_LockMutex(mutex);
  pkts.push_back(*pkt);
  size += pkt->size;
  SDL_CondSignal(cond);
  SDL_UnlockMutex(mutex);

  return 0;
}

int PacketQueue::packetGet(AVPacket *pkt, std::atomic<bool> &quit) {
  int ret = 0;
  SDL_LockMutex(mutex);
  for (;;) {
    if (!pkts.empty()) {
      AVPacket &firstPkt = pkts.front();
      pkts.erase(pkts.begin());
      size -= firstPkt.size;
      *pkt = firstPkt;
      ret = 1;
      break;
    } else {
      SDL_CondWaitTimeout(cond, mutex, 500);
    }
    if (quit) {
      ret = -1;
      break;
    }
  }
  SDL_UnlockMutex(mutex);
  return ret;
}

void PacketQueue::packetFlush() {
  SDL_LockMutex(mutex);
  for (auto p : pkts) {
    AVPacket &pkt = p;
    av_packet_unref(&pkt);
  }
  pkts.clear();
  size = 0;
  SDL_UnlockMutex(mutex);
}

int PacketQueue::packetSize() { return size; }
