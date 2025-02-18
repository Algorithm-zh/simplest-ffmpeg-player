#include <stdio.h>

#include "SDL.h"

extern "C" {
#include "libavutil/time.h"
#include <libavformat/avformat.h>
}

char av_error[AV_ERROR_MAX_STRING_SIZE] = {0};
#define av_err2str(errnum)                                                     \
  av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

#define MAX_VIDEO_PIC_NUM 1 // 最大缓存解码图片数

// 队列
typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  SDL_mutex *mutex;
} PacketQueue;

// 音视频同步时钟模式
enum {
  AV_SYNC_AUDIO_MASTER, /* default choice */
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

static Uint8 *audio_chunk;
static Uint32 audio_len;
static Uint8 *audio_pos;

SDL_Window *sdlWindow = nullptr;
SDL_Renderer *sdlRender = nullptr;
SDL_Texture *sdlTtexture = nullptr;

AVFormatContext *ifmt_ctx = NULL;
AVPacket *pkt;
AVFrame *video_frame, *audio_frame;
int ret;
int videoindex = -1, audioindex = -1;
// const char* in_filename = "D:/工程/音视频分析/source/间谍过家家16.mp4";
// //beautlWorld.mp4";
const char *in_filename = "Titanic.ts";

int frame_width = 1280;
int frame_height = 720;

// 视频解码
AVCodec *video_codec = nullptr;
AVCodecContext *video_codecContent = nullptr;

typedef struct video_pic {
  AVFrame frame;

  float clock;    // 显示时钟
  float duration; // 持续时间
  int frame_NUM;  // 帧号
} video_pic;

video_pic v_pic[MAX_VIDEO_PIC_NUM]; // 视频解码最多保存四帧数据
int pic_count = 0;                  // 以存储图片数量

// 音频解码
AVCodec *audio_codec = nullptr;
AVCodecContext *audio_codecContent = nullptr;

// 视频帧队列
PacketQueue video_pkt_queue;
PacketQueue audio_pkt_queue;

// 同步时钟设置为音频为主时钟
int av_sync_type = AV_SYNC_AUDIO_MASTER;

int64_t audio_callback_time;
float audio_clock;
float video_clock;

// SDL音频参数结构
SDL_AudioSpec wanted_spec;

int initSdl();
void closeSDL();
void fill_audio_pcm2(void *udata, Uint8 *stream, int len);

// fltp转为packed形式
void fltp_convert_to_f32le(float *f32le, float *fltp_l, float *fltp_r,
                           int nb_samples, int channels) {
  for (int i = 0; i < nb_samples; i++) {
    f32le[i * channels] = fltp_l[i];
    f32le[i * channels + 1] = fltp_r[i];
  }
}

// 将一个压缩数据包放入相应的队列中
void put_AVPacket_into_queue(PacketQueue *q, AVPacket *packet) {
  SDL_LockMutex(q->mutex);
  AVPacketList *temp = nullptr;
  temp = (AVPacketList *)av_malloc(sizeof(AVPacketList));
  if (!temp) {
    printf("malloc a AVPacketList error\n");
    return;
  }

  temp->pkt = *packet;
  temp->next = NULL;

  if (!q->last_pkt)
    q->first_pkt = temp;
  else
    q->last_pkt->next = temp;

  q->last_pkt = temp;
  q->nb_packets++;

  SDL_UnlockMutex(q->mutex);
}

static void packet_queue_get(PacketQueue *q, AVPacket *pkt2) {
  while (true) {
    AVPacketList *pkt1;
    // 一直取，直到队列中有数据，就返回
    pkt1 = q->first_pkt;
    if (pkt1) {
      SDL_LockMutex(q->mutex);
      q->first_pkt = pkt1->next;

      if (!q->first_pkt)
        q->last_pkt = NULL;

      q->nb_packets--;
      SDL_UnlockMutex(q->mutex);

      *pkt2 = pkt1->pkt;

      av_free(pkt1);

      break;

    } else
      SDL_Delay(1);
  }

  return;
}

int delCunt = 0;
int video_play_thread(void *data) {
  AVPacket video_packt = {0};
  // 取数据
  while (true) {
    // 取数据包

    packet_queue_get(&video_pkt_queue, &video_packt);

    ret = avcodec_send_packet(video_codecContent, &video_packt);
    if (ret < 0) {
      fprintf(stderr, "Error sending a packet for decoding\n", av_err2str(ret));
      continue;
    }

    while (ret >= 0) {
      ret = avcodec_receive_frame(video_codecContent, video_frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      else if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        break;
      }
      // printf(" frame num %3d\n", video_codecContent->frame_number);
      fflush(stdout);

      video_clock = av_q2d(video_codecContent->time_base) *
                    video_codecContent->ticks_per_frame * 1000 *
                    video_codecContent->frame_number;
      // printf("视频帧pts: %f ms\n", video_clock);
      float duration = av_q2d(video_codecContent->time_base) *
                       video_codecContent->ticks_per_frame * 1000;

      SDL_UpdateYUVTexture(sdlTtexture, nullptr, video_frame->data[0],
                           video_frame->linesize[0], video_frame->data[1],
                           video_frame->linesize[1], video_frame->data[2],
                           video_frame->linesize[2]);

      // 清理渲染器缓冲区
      SDL_RenderClear(sdlRender);
      // 将纹理拷贝到窗口渲染平面上
      SDL_RenderCopy(sdlRender, sdlTtexture, NULL, nullptr);
      // 翻转缓冲区，前台显示
      SDL_RenderPresent(sdlRender);

      // 延时处理
      float delay = duration;
      float diff = video_clock - audio_clock;
      if (fabs(diff) <= duration) // 时间差在一帧范围内表示正常，延时正常时间
        delay = duration;
      else if (diff >
               duration) // 视频时钟比音频时钟快，且大于一帧的时间，延时2倍
        delay *= 2;
      else if (
          diff <
          -duration) // 视频时钟比音频时钟慢，且超出一帧时间，立即播放当前帧
        delay = 1;

      printf("帧数：%d 延时: %f\n", video_codecContent->frame_number, delay);

      SDL_Delay(delay);
    }
  }
}

int audio_play_thread(void *data) {
  AVPacket audio_packt = {0};

  while (true) {
    packet_queue_get(&audio_pkt_queue, &audio_packt);

    ret = avcodec_send_packet(audio_codecContent, &audio_packt);
    if (ret < 0) {
      fprintf(stderr, "Error submitting the packet to the decoder\n");
      exit(1);
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
      // 接到解码后的<AVPacket数据>，读取到AVFrame中
      ret = avcodec_receive_frame(audio_codecContent, audio_frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      else if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        break;
      }
      /*下面是得到解码后的裸流数据进行处理，根据裸流数据的特征做相应的处理，如AAC解码后是PCM，h264解码后是YUV等。*/
      int data_size = av_get_bytes_per_sample(audio_codecContent->sample_fmt);
      if (data_size < 0) {
        /* This should not occur, checking just for paranoia */
        fprintf(stderr, "Failed to calculate data size\n");
        break;
      }

      // 转换为packed模式
      int pcm_buffer_size =
          data_size * audio_frame->nb_samples * audio_codecContent->channels;
      uint8_t *pcm_buffer = (uint8_t *)malloc(pcm_buffer_size);
      memset(pcm_buffer, 0, pcm_buffer_size);

      fltp_convert_to_f32le((float *)pcm_buffer, (float *)audio_frame->data[0],
                            (float *)audio_frame->data[1],
                            audio_frame->nb_samples,
                            audio_codecContent->channels);
      // 使用SDL播放
      // Set audio buffer (PCM data)
      audio_chunk = pcm_buffer; //(Uint8*)audio_frame->data[0];
      audio_chunk = pcm_buffer;
      audio_len = pcm_buffer_size;
      audio_pos = audio_chunk;

      audio_clock =
          audio_frame->pts * av_q2d(audio_codecContent->time_base) * 1000;
      // printf("音频帧pts: %f ms\n", audio_clock);

      while (audio_len > 0) // Wait until finish
        SDL_Delay(1);

      // SDL_Delay(1000);
      free(pcm_buffer);
    }
  }

  return 0;
}

int open_file_thread(void *data) {
  // 读取
  while (av_read_frame(ifmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == videoindex) {

      // 加入视频队列
      put_AVPacket_into_queue(&video_pkt_queue, pkt);
    } else if (pkt->stream_index == audioindex) {
      // 加入音频队列
      put_AVPacket_into_queue(&audio_pkt_queue, pkt);
    } else
      av_packet_unref(pkt);
  }

  return 0;
}

int main(int argc, char *argv[]) {
  if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
    printf("Could not open input file.");
    return -1;
  }

  if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
    printf("Failed to retrieve input stream information");
    return -1;
  }

  videoindex = -1;
  for (int i = 0; i < ifmt_ctx->nb_streams; i++) { // nb_streams：视音频流的个数
    if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
      videoindex = i;
    else if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      audioindex = i;
  }

  printf("\nInput Video===========================\n");
  av_dump_format(ifmt_ctx, 0, in_filename, 0); // 打印信息
  printf("\n======================================\n");

  // 根据编解码器id查询解码器并返回解码器结构
  video_codec =
      avcodec_find_decoder(ifmt_ctx->streams[videoindex]->codecpar->codec_id);

  // 分配解码器上下文
  video_codecContent = avcodec_alloc_context3(video_codec);
  // 拷贝视频流信息到视频编码器上下文中
  avcodec_parameters_to_context(video_codecContent,
                                ifmt_ctx->streams[videoindex]->codecpar);

  frame_width = ifmt_ctx->streams[videoindex]->codecpar->width;
  frame_height = ifmt_ctx->streams[videoindex]->codecpar->height;
  // 打开视频解码器和关联解码器上下文
  if (avcodec_open2(video_codecContent, video_codec, nullptr)) {
    printf("could not open codec!\n");
    return -1;
  }

  // 根据编解码器id查询解码器并返回解码器结构
  audio_codec =
      avcodec_find_decoder(ifmt_ctx->streams[audioindex]->codecpar->codec_id);

  // 分配解码器上下文
  audio_codecContent = avcodec_alloc_context3(audio_codec);
  // 拷贝音频流信息到音频编码器上下文中
  avcodec_parameters_to_context(audio_codecContent,
                                ifmt_ctx->streams[audioindex]->codecpar);
  // 打开音频解码器和关联解码器上下文
  if (avcodec_open2(audio_codecContent, audio_codec, nullptr)) {
    printf("could not open codec!\n");
    return -1;
  }

  // 申请一个AVPacket结构
  pkt = av_packet_alloc();

  // 申请一个AVFrame 结构用来存放解码后的数据
  video_frame = av_frame_alloc();
  audio_frame = av_frame_alloc();

  // 初始化SDL
  initSdl();

  video_pkt_queue.mutex = SDL_CreateMutex();
  audio_pkt_queue.mutex = SDL_CreateMutex();

  // 设置SDL音频播放参数
  wanted_spec.freq = audio_codecContent->sample_rate; // 采样率
  wanted_spec.format = AUDIO_F32LSB; // audio_codecContent->sample_fmt;
                                     // 这里需要转换为sdl的采样格式
  wanted_spec.channels = audio_codecContent->channels; // 通道数
  wanted_spec.silence = 0;
  wanted_spec.samples = audio_codecContent->frame_size; // 每一帧的采样点数量
  wanted_spec.callback = fill_audio_pcm2;               // 音频播放回调

  // 打开系统音频设备
  if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
    printf("can't open audio.\n");
    return -1;
  }
  // Play
  SDL_PauseAudio(0);

  SDL_CreateThread(open_file_thread, "open_file", nullptr);
  SDL_CreateThread(video_play_thread, "video_play", nullptr);
  SDL_CreateThread(audio_play_thread, "audio_play", nullptr);

  bool quit = false;
  SDL_Event e;
  while (quit == false) {
    while (SDL_PollEvent(&e) != 0) {
      if (e.type == SDL_QUIT) {
        quit = true;
        break;
      }
    }
  }

  SDL_DestroyMutex(video_pkt_queue.mutex);
  SDL_DestroyMutex(audio_pkt_queue.mutex);

  // 释放ffmpeg指针
  avformat_close_input(&ifmt_ctx);
  avcodec_free_context(&video_codecContent);
  avcodec_free_context(&audio_codecContent);
  av_frame_free(&audio_frame);
  av_frame_free(&video_frame);
  av_packet_free(&pkt);
  return 0;
}

// sdl初始化
int initSdl() {
  bool success = true;

  if (SDL_Init(SDL_INIT_VIDEO)) {
    printf("init sdl error:%s\n", SDL_GetError());
    success = false;
  }

  // 创建window
  sdlWindow = SDL_CreateWindow("decode video", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, frame_width,
                               frame_height, SDL_WINDOW_SHOWN);
  if (sdlWindow == nullptr) {
    printf("create window error: %s\n", SDL_GetError());
    success = false;
  }

  // 创建渲染器
  sdlRender = SDL_CreateRenderer(sdlWindow, -1, 0);
  if (sdlRender == nullptr) {
    printf("init window Render error: %s\n", SDL_GetError());
    success = false;
  }

  // 构建合适的纹理
  sdlTtexture =
      SDL_CreateTexture(sdlRender, SDL_PIXELFORMAT_IYUV,
                        SDL_TEXTUREACCESS_STREAMING, frame_width, frame_height);

  return success;
}

// sdl音频回调
void fill_audio_pcm2(void *udata, Uint8 *stream, int len) {

  // 获取当前系统时钟
  audio_callback_time = av_gettime();

  // SDL 2.0
  SDL_memset(stream, 0, len);

  if (audio_len == 0)
    return;
  len = (len > audio_len ? audio_len : len);

  SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);
  audio_len -= len;
}

void closeSDL() {

  SDL_DestroyWindow(sdlWindow);
  sdlWindow = nullptr;
  SDL_DestroyRenderer(sdlRender);
  sdlRender = nullptr;
  SDL_DestroyTexture(sdlTtexture);
  sdlTtexture = nullptr;
}
