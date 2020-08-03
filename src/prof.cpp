
/* Copyright (c) 2020, Peter Barrett
**
** Permission to use, copy, modify, and/or distribute this software for
** any purpose with or without fee is hereby granted, provided that the
** above copyright notice and this permission notice appear in all copies.
**
** THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
** WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
** WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR
** BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
** OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
** WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
** ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
** SOFTWARE.
*/

#include "streamer.h"

//================================================================================
//================================================================================
// Profiling - incredibly handy during development
//
// Sampling Profile - trace the PC of selected range from ISR to create a sampling profiler
// Tasks - Record what task was running during Video ISR to create a profile
// PLog - log realtime important playback events

#if 1
extern void* _trace_min;
extern uint16_t _tb[1280];
void trace_flush()
{
  printf(".on %08X\n",_trace_min);
  for (int i = 0; i < sizeof(_tb)/2; i++) {
    if (_tb[i] > 5)
      printf(".%d %d\n",i,_tb[i]);
  }
  printf(".off %08X\n",_trace_min);
}

TaskHandle_t _tasks[32] = {0};
uint32_t _cores[32*2];

IRAM_ATTR
void task_prof()
{
  for (int n = 0; n < 2; n++)
  {
      auto c = xTaskGetCurrentTaskHandleForCPU(n);
      for (int i = 0; i < 32; i++)
      {
        if (!_tasks[i])
          _tasks[i] = c;
        if (_tasks[i] == c) {
          _cores[(i<<1) + n]++;
          break;
        }
      }
  }
}

void task_dump()
{
  uint32_t c0 = 1;
  uint32_t c1 = 1;
  for (int i = 0; i < 32; i++) {
    if (!_tasks[i])
     break;
    c0 += _cores[i<<1];
    c1 += _cores[(i<<1)+1];
  }
  for (int i = 0; i < 32; i++) {
    if (!_tasks[i])
      break;
    printf("%3d%% %3d%% %s\n",100*_cores[i<<1]/c0,100*_cores[(i<<1)+1]/c1,pcTaskGetTaskName(_tasks[i]));
    _cores[i<<1] = _cores[(i<<1)+1] = 0;
  }
}

#define PLOG_SIZE 1024
uint32_t _plog[PLOG_SIZE];
uint32_t _plog_w = 1;

void plog_flush()
{
  uint32_t w = _plog_w;
  _plog_w = 0;
  uint32_t n = w < PLOG_SIZE ? w : PLOG_SIZE;
  uint32_t i = w - n;
  printf("!-plog\n");
  while (n--)
    printf("!%08X\n",_plog[i++ & (PLOG_SIZE-1)]);
  printf("!-\n");
  _plog_w = w;
}

void plog(int x)
{
  if (!_plog_w)
    return;
  x = (x << 1) | xPortGetCoreID();
  _plog[_plog_w++ & (PLOG_SIZE-1)] = (xthal_get_ccount() & 0xFFFFFF00) | x;
}

void mem(const char* t)
{
    //int n = uxTaskGetStackHighWaterMark(NULL);
    printf("# mem 8:%6d (max %6d) 32:%6d (max %6d) %s\n",
         heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
         heap_caps_get_free_size(MALLOC_CAP_32BIT),heap_caps_get_largest_free_block(MALLOC_CAP_32BIT),t);
}

void * operator new(size_t size)
{
    void *ptr = malloc(size);
    printf("# new %d %08X %08X\n",size,__builtin_return_address(1),__builtin_return_address(2));
        mem("new");
    return ptr;
}

void operator delete(void * ptr) noexcept
{
    //printf("delete\n");
    free(ptr);
}

void * operator new[](size_t size)
{
    void *ptr = malloc(size);
    printf("# new[] %d %08X %08X\n",size,__builtin_return_address(1),__builtin_return_address(2));
    mem("new");
    return ptr;
}

void operator delete[](void * ptr) noexcept
{
    //printf("delete[]\n");
    free(ptr);
}

void up_key()
{
  task_dump();
}

void down_key()
{
  plog_flush();
}

#else

void up_key()
{
}

void down_key()
{
}

#endif
