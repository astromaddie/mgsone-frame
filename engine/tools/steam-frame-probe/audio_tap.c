#include <SDL2/SDL.h>
#include <vulkan/vulkan.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static SDL_AudioCallback original;
static void *original_data;
static int output = -1;
static uint64_t callbacks, frames, zeros, run, longest, max_us;
static atomic_uint_fast64_t submits, vertices;
static atomic_uint viewport_width, viewport_height;
static uint64_t audio_start, video_start, video_end, submit_start, submit_end;
static uint64_t vertex_start, vertex_end;

VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
  PFN_vkQueueSubmit next=(PFN_vkQueueSubmit)dlsym(RTLD_NEXT,"vkQueueSubmit");
  VkResult result=next(queue,count,info,fence);
  if(result==VK_SUCCESS) atomic_fetch_add(&submits,1);
  return result;
}
void vkCmdDraw(VkCommandBuffer command,uint32_t count,uint32_t instances,uint32_t first,uint32_t first_instance) {
  PFN_vkCmdDraw next=(PFN_vkCmdDraw)dlsym(RTLD_NEXT,"vkCmdDraw");
  atomic_fetch_add(&vertices,(uint64_t)count*instances);
  next(command,count,instances,first,first_instance);
}
void vkCmdSetViewport(VkCommandBuffer command,uint32_t first,uint32_t count,const VkViewport *views) {
  PFN_vkCmdSetViewport next=(PFN_vkCmdSetViewport)dlsym(RTLD_NEXT,"vkCmdSetViewport");
  if(count) { atomic_store(&viewport_width,(unsigned)views[0].width); atomic_store(&viewport_height,(unsigned)views[0].height); }
  next(command,first,count,views);
}
static void tap(void *unused, Uint8 *stream, int len) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  original(original_data, stream, len);
  clock_gettime(CLOCK_MONOTONIC, &b);
  const int64_t us = (b.tv_sec-a.tv_sec)*1000000LL + (b.tv_nsec-a.tv_nsec)/1000;
  if (us > (int64_t)max_us) max_us=us;
  const uint64_t now=(uint64_t)b.tv_sec*1000000000+b.tv_nsec;
  if(!audio_start) audio_start=now;
  if(!video_start && now-audio_start>=2000000000 && atomic_load(&submits)) {
    video_start=now; submit_start=atomic_load(&submits); vertex_start=atomic_load(&vertices);
  }
  if(video_start) { video_end=now; submit_end=atomic_load(&submits); vertex_end=atomic_load(&vertices); }
  ++callbacks;
  const int16_t *s=(const int16_t *)stream;
  for (int i=0;i<len/4;++i) {
    ++frames;
    if (!s[i*2] && !s[i*2+1]) { ++zeros; ++run; if(run>longest)longest=run; }
    else run=0;
  }
  if(output>=0 && write(output,stream,len)!=len) _exit(70);
}
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *name, int capture,
    const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int changes) {
  typedef SDL_AudioDeviceID (*Open)(const char *,int,const SDL_AudioSpec *,SDL_AudioSpec *,int);
  Open next=(Open)dlsym(RTLD_NEXT,"SDL_OpenAudioDevice");
  const char *path=getenv("MGS_AUDIO_TAP");
  if(capture || !path || !desired->callback) return next(name,capture,desired,obtained,changes);
  if(original || desired->format!=AUDIO_S16SYS || desired->channels!=2) _exit(71);
  original=desired->callback; original_data=desired->userdata;
  output=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);
  if(output<0) _exit(72);
  SDL_AudioSpec spec=*desired; spec.callback=tap; spec.userdata=NULL;
  return next(name,capture,&spec,obtained,changes);
}
__attribute__((destructor)) static void report(void) {
  if(output<0) return;
  close(output);
  if(video_end>video_start && submit_end>submit_start) {
    const double seconds=(video_end-video_start)*1e-9;
    fprintf(stderr,"[Video tap] steady_submits=%llu seconds=%.6f rate_hz=%.3f viewport=%ux%u vertices_per_submit=%.1f\n",
      (unsigned long long)(submit_end-submit_start),seconds,(submit_end-submit_start)/seconds,
      atomic_load(&viewport_width),atomic_load(&viewport_height),
      (double)(vertex_end-vertex_start)/(submit_end-submit_start));
  }
  fprintf(stderr,"[Audio tap] callbacks=%llu frames=%llu zero_frames=%llu longest_zero_run=%llu callback_max_us=%llu\n",
      (unsigned long long)callbacks,(unsigned long long)frames,(unsigned long long)zeros,
      (unsigned long long)longest,(unsigned long long)max_us);
}
