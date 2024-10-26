#include "PackQueue.h"
#include "SDL_render.h"
#include "SDL_timer.h"
#include "SDL_video.h"
#include "frame.h"
#include "packet.h"
#include <stdio.h>

#define __STDC_CONSTANT_MACROS

#ifdef __cplusplus
extern "C" {
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pthread.h>
};
#endif

// Refresh Event
#define SFM_REFRESH_EVENT (SDL_USEREVENT + 1)
#define SFM_END_OF_FILE_EVENT (SDL_USEREVENT + 3)
#define SFM_BREAK_EVENT (SDL_USEREVENT + 2)
int file_reading_done;
// 封装格式上下文的结构体，也是统领全局的结构体，保存了视频文件封装格式的相关信息
AVFormatContext *pFormatCtx;

// 视频流在文件中的位置
int i, videoindex;

// 音频流在文件中的位置-----------------------
int audioindex;

// 编码器上下文结构体，保存了视频（音频）编解码相关信息
AVCodecContext *pCodecCtx;

// 音频编解码上下文----------------------------------
AVCodecContext *aCodecCtx;

// 每种视频（音频）编解码器（例如H.264解码器）对应一个该结构体
AVCodec *pCodec;

// 音频编解码器-----------------------
AVCodec *aCodec;

// 存储一帧解码后像素（采样）数据
AVFrame *pFrame, *pFrameYUV;

// 音频-------------------------------
AVFrame *aFrame;

// 存储图像数据
unsigned char *out_buffer;

// 存储一帧压缩编码数据
AVPacket *packet;

// 音频------------------------------
AVPacket *apacket;

// 视频packet队列
PacketQueue *video_pkt_queue;

// 音频packet队列
PacketQueue *audio_pkt_queue;

// 是否获取到数据的返回值
int ret;

//------------SDL----------------
int screen_w, screen_h;

// 窗口
SDL_Window *screen;

// 渲染器
SDL_Renderer *sdlRenderer;

// 纹理
SDL_Texture *sdlTexture;

// 一个简单的矩形
SDL_Rect sdlRect;

// 线程
SDL_Thread *video_tid;

// 事件
SDL_Event event;

struct SwsContext *img_convert_ctx;

// 音频转换上下文------------------
SwrContext *swr_ctx = nullptr;

// char filepath[]="bigbuckbunny_480x272.h265";
char filepath[] = "Titanic.ts";

SDL_AudioDeviceID deviceId;

SDL_AudioSpec audioSpec;

int thread_exit = 0;
int thread_pause = 0;

float video_clock;
float audio_clock;
uint8_t *pcm_data;
int pcm_size;
int audio_pos = 0;
SDL_mutex *audio_mutex = nullptr;
SDL_mutex *video_mutex = nullptr;

void audio_callback(void *userdata, uint8_t *stream, int len) {

  if (pcm_size <= 0)
    return;

  // 数据大小不能超过 len
  len = len > pcm_size ? pcm_size : len;

  // 将 stream 和 audio_pos 进行混合播放
  // SDL_MixAudio(stream, pcm_data, len, SDL_MIX_MAXVOLUME);

  // 单独播放 pcm_data,也就是解码出来的音频数据
  memcpy(stream, pcm_data, len);
  pcm_size -= len;
}

// 视频处理线程
int video_thread(void *arg) {
  std::atomic<bool> quit = 0;
  AVPacket *packet = av_packet_alloc();
  AVFrame *pFrame = av_frame_alloc();
  AVFrame *pFrameYUV = av_frame_alloc();
  // av_image_get_buffer_size：返回使用给定参数存储图像所需的数据量的字节大小
  out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(
      AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1));

  // 根据指定的图像参数和提供的数组设置数据指针和线条（data pointers and
  // linesizes）
  av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer,
                       AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height,
                       1);
  screen_w = pCodecCtx->width;
  screen_h = pCodecCtx->height;

  // 创建窗口SDL_CreateWindow
  screen = SDL_CreateWindow("Simplest ffmpeg player's Window",
                            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                            screen_w, screen_h, SDL_WINDOW_OPENGL);
  if (!screen) {
    printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
    return -1;
  }
  sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
  sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV,
                                 SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width,
                                 pCodecCtx->height);
  while (!thread_exit) {
    if (file_reading_done && !video_pkt_queue->packetSize()) {
      SDL_Event event;
      event.type = SFM_END_OF_FILE_EVENT;
      SDL_PushEvent(&event);
      break;
    }
    if (video_pkt_queue->packetGet(packet, quit) <= 0) {
      printf("取视频packet失败\n");
      break;
    }
    int ret = avcodec_send_packet(pCodecCtx, packet);
    if (ret == 0) {
      ret = avcodec_receive_frame(pCodecCtx, pFrame);
    }
    if (ret < 0) {
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        continue;
      } else {
        printf("Error receiving frame from decoder: %s\n", av_err2str(ret));
        break;
      }
    }
    if (ret == 0) {
      video_clock = av_q2d(pCodecCtx->time_base) * pCodecCtx->ticks_per_frame *
                    1000 * pCodecCtx->frame_number;
      // printf("视频帧pts: %f ms\n", video_clock);
      float duration =
          av_q2d(pCodecCtx->time_base) * pCodecCtx->ticks_per_frame * 1000;

      // 延时处理
      float delay = duration;
      float diff = video_clock - audio_clock;
      if (fabs(diff) <= duration) //
        // 时间差在一帧范围内表示正常，延时正常时间
        delay = duration;
      else if (diff > duration) //
        // 视频时钟比音频时钟快，且大于一帧的时间，延时2倍
        delay *= 2;
      else if (diff < -duration) //
        // 视频时钟比音频时钟慢，且超出一帧时间，立即播放当前帧
        delay = 1;

      sws_scale(img_convert_ctx, (const unsigned char *const *)pFrame->data,
                pFrame->linesize, 0, pCodecCtx->height, pFrameYUV->data,
                pFrameYUV->linesize);

      SDL_UpdateTexture(sdlTexture, NULL, pFrameYUV->data[0],
                        pFrameYUV->linesize[0]);
      SDL_RenderClear(sdlRenderer);
      SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
      SDL_RenderPresent(sdlRenderer);
      SDL_Delay(40);
    }
  }
  av_packet_free(&packet);
  av_frame_free(&pFrame);
  av_frame_free(&pFrameYUV);

  return 0;
}

// 音频处理线程
int audio_thread(void *arg) {
  std::atomic_bool quit = 0;
  AVPacket *packet = av_packet_alloc();
  while (!thread_exit) {
    if (file_reading_done && !audio_pkt_queue->packetSize()) {
      SDL_Event event;
      event.type = SFM_END_OF_FILE_EVENT;
      SDL_PushEvent(&event);
      break;
    }
    if (audio_pkt_queue->packetGet(packet, quit) <= 0) {
      printf("取音频packet失败\n");
      break;
    }
    int ret = avcodec_send_packet(aCodecCtx, packet);
    if (ret == 0) {
      ret = avcodec_receive_frame(aCodecCtx, aFrame);
    }
    if (ret < 0) {
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        continue;
      } else {
        printf("Error receiving audio frame from decoder: %s\n",
               av_err2str(ret));
        break;
      }
    }
    if (ret == 0) {
      int upper_bound_samples =
          swr_get_out_samples(swr_ctx, aFrame->nb_samples);
      uint8_t *out[4] = {0};
      out[0] = (uint8_t *)av_malloc(upper_bound_samples * 2 * 2);

      int samples =
          swr_convert(swr_ctx, (uint8_t **)out, upper_bound_samples,
                      (const uint8_t **)aFrame->data, aFrame->nb_samples);

      pcm_size = av_samples_get_buffer_size(NULL, aCodecCtx->channels, samples,
                                            AV_SAMPLE_FMT_S16, 1);
      if (pcm_size < 0) {
        printf("ERROR getting buffer size");
        continue;
      }
      if (pcm_data == NULL) {
        pcm_data = (uint8_t *)av_malloc(pcm_size);
      }

      audio_clock = aFrame->pts * av_q2d(aCodecCtx->time_base) * 1000;

      memcpy(pcm_data, out[0], pcm_size);

      while (pcm_size > 0) {
        SDL_Delay(1);
      }
      av_free(out[0]);
      // av_packet_unref(packet);
    }
  }
  av_packet_free(&packet);
  return 0;
}

int open_file_thread(void *data) {
  AVPacket *packet = av_packet_alloc();
  while (av_read_frame(pFormatCtx, packet) >= 0) {
    if (packet->stream_index == videoindex) {
      video_pkt_queue->packetPut(packet);
    } else if (packet->stream_index == audioindex) {
      audio_pkt_queue->packetPut(packet);
    } else {
      av_packet_unref(packet);
    }
  }
  av_packet_free(&packet);
  // 文件读取完毕，设置标志位并发送自定义事件
  file_reading_done = 1;
  return 0;
}

int main(int argc, char *argv[]) {
  pFormatCtx = avformat_alloc_context();
  // 打开多媒体数据并且获得一些相关的信息（参考FFmpeg解码流程图）
  if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0) {
    printf("Couldn't open input stream.\n");
    return -1;
  }
  // 读取一部分视音频数据并且获得一些相关的信息（参考FFmpeg解码流程图）
  if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
    printf("Couldn't find stream information.\n");
    return -1;
  }

  //	新方法直接得到视频流下标和解码器
  videoindex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1,
                                   (AVCodec **)&pCodec, 0);
  // 获取音频流下标----------------------------------------
  audioindex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1,
                                   (AVCodec **)&aCodec, 0);

  // 如果没有视频流，返回
  if (videoindex == -1) {
    printf("Didn't find a video stream.\n");
    return -1;
  }
  if (audioindex == -1) {
    printf("Didn't find a audio stream.\n");
    return -1;
  }

  // 新方法初始化解码器上下文
  pCodecCtx = avcodec_alloc_context3(pCodec);
  avcodec_parameters_to_context(pCodecCtx,
                                pFormatCtx->streams[videoindex]->codecpar);

  aCodecCtx = avcodec_alloc_context3(aCodec);
  avcodec_parameters_to_context(aCodecCtx,
                                pFormatCtx->streams[audioindex]->codecpar);

  // 初始化一个视音频编解码器的AVCodecContext（参考FFmpeg解码流程图）
  if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
    printf("Could not open video codec.\n");
    return -1;
  }
  if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0) {
    printf("Could not open audio codec.\n");
    return -1;
  }

  // 创建AVFrame，用来存放解码后的一帧的数据
  pFrame = av_frame_alloc();
  pFrameYUV = av_frame_alloc();

  aFrame = av_frame_alloc();

  // av_image_get_buffer_size：返回使用给定参数存储图像所需的数据量的字节大小
  out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(
      AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1));

  // 根据指定的图像参数和提供的数组设置数据指针和线条（data pointers and
  // linesizes）
  av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer,
                       AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height,
                       1);

  // Output Info-----------------------------
  printf("---------------- File Information ---------------\n");
  av_dump_format(pFormatCtx, 0, filepath, 0);
  printf("-------------------------------------------------\n");

  swr_ctx = swr_alloc();
  printf("channels 数量：%d\n", aCodecCtx->channels);
  // 音频设置
  av_opt_set_int(swr_ctx, "in_channel_count", aCodecCtx->channels, 0);

  // 设置输入采样率
  av_opt_set_int(swr_ctx, "in_sample_rate", aCodecCtx->sample_rate, 0);
  // 设置输入样本格式
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", aCodecCtx->sample_fmt, 0);

  // 输出声道布局
  av_opt_set_int(swr_ctx, "out_channel_count", 2, 0);
  // 设置输出采样率为 44100 Hz
  av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
  // 设置输出样本格式为 16 位整数
  av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

  if (swr_init(swr_ctx) < 0) {
    printf("Faild to init the resampling context");
  }

  // 初始化SDL系统
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    printf("Could not initialize SDL - %s\n", SDL_GetError());
    return -1;
  }

  audioSpec.freq = 48000;
  audioSpec.format = AUDIO_S16SYS;
  audioSpec.channels = 2;
  audioSpec.silence = 0;
  audioSpec.samples = 1024;
  audioSpec.callback = audio_callback;
  audioSpec.userdata = nullptr;

  // sws_getContext()：初始化一个SwsContext
  img_convert_ctx = sws_getContext(
      pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width,
      pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
  //

  // 初始化队列存放packet
  video_pkt_queue = new PacketQueue();
  audio_pkt_queue = new PacketQueue();

  deviceId = SDL_OpenAudioDevice(nullptr, 0, &audioSpec, nullptr, 0);
  if (deviceId <= 0) {
    printf("Faild to open audio device\n");
    return -1;
  }
  SDL_PauseAudioDevice(deviceId, 0);

  // 创建线程
  SDL_CreateThread(open_file_thread, "open_file", nullptr);
  SDL_CreateThread(video_thread, "video_play", nullptr);
  SDL_CreateThread(audio_thread, "audio_play", nullptr);

  // 主循环
  SDL_Event event;
  while (!thread_exit) {
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT:
        thread_exit = 1;
        break;
      case SDL_KEYDOWN:
        if (event.key.keysym.sym == SDLK_SPACE) {
          thread_pause = !thread_pause;
        }
        break;
      case SFM_END_OF_FILE_EVENT:
        thread_exit = 1;
        break;
      }
    }
  }
  // 等待线程结束

end:
  SDL_PauseAudioDevice(deviceId, 1);
  SDL_DestroyWindow(screen);
  SDL_DestroyRenderer(sdlRenderer);
  SDL_DestroyTexture(sdlTexture);
  free(pcm_data);

  SDL_Quit();
  //--------------
  av_frame_free(&pFrameYUV);
  av_frame_free(&pFrame);
  av_packet_free(&packet);
  avcodec_close(pCodecCtx);
  avformat_close_input(&pFormatCtx);

  av_frame_free(&aFrame);
  av_packet_free(&apacket);
  avcodec_close(aCodecCtx);
  SDL_DestroyMutex(audio_mutex);

  return 0;
}
