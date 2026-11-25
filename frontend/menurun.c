/* Koriki: while the menu is open in a link session the game keeps running
 * (no picture, no sound, no buttons) in a thread, so the other console does
 * not lose the cable. menu_run_begin() takes the core for the menu; the menu
 * gives it to the thread only while it waits for a key: libpicofe waits with
 * plat_sleep_ms(), wrapped here (-Wl,--wrap=plat_sleep_ms). Without a link
 * session it is the old pause. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "libretro.h"
#include "frontend/net.h"
#include "frontend/menurun.h"

void __real_plat_sleep_ms(int ms);

volatile int menu_run_active;          /* main.c: no buttons, no sound */

static pthread_mutex_t core_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static volatile int thread_on;
static int locked;

/* someone on the other end of the cable */
static int link_peer(void)
{
  net_state_t st = net_state();
  return st == NET_CONNECTED || (st == NET_HOSTING && net_clients() > 0);
}

static void *run_thread(void *arg)
{
  struct timespec next;
  unsigned frames = 0;
  (void)arg;
  clock_gettime(CLOCK_MONOTONIC, &next);
  while (thread_on) {
    pthread_mutex_lock(&core_mx);
    if (thread_on) {
      net_poll();
      if (link_peer()) {
        retro_run();
        frames++;
      }
    }
    pthread_mutex_unlock(&core_mx);
    /* one GBA frame: 280896 cycles at 16777216 Hz = 16742.7 us */
    next.tv_nsec += 16742706;
    if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
    {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > next.tv_sec + 1)   /* far behind: do not race to catch up */
        next = now;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
  }
  if (getenv("KORIKI_MENU_DEBUG"))
    fprintf(stderr, "menu: %u frames run behind the menu\n", frames);
  return NULL;
}

/* starts the thread (the menu holds the core) */
static void thread_start(void)
{
  if (thread_on || !link_peer())
    return;
  menu_run_active = 1;
  thread_on = 1;
  if (pthread_create(&thread, NULL, run_thread, NULL) != 0) {
    thread_on = 0;
    menu_run_active = 0;
  }
}

void menu_run_begin(void)
{
  pthread_mutex_lock(&core_mx);
  locked = 1;
  thread_start();
}

void menu_run_end(void)
{
  if (thread_on) {
    thread_on = 0;
    pthread_mutex_unlock(&core_mx);     /* let the thread see it and finish */
    pthread_join(thread, NULL);
    pthread_mutex_lock(&core_mx);
    menu_run_active = 0;
  }
  if (locked) {
    locked = 0;
    pthread_mutex_unlock(&core_mx);
  }
}

void __wrap_plat_sleep_ms(int ms)
{
  if (locked) {
    if (!thread_on && (net_state() == NET_HOSTING || net_state() == NET_CONNECTING))
      net_poll();
    thread_start();                     /* someone may have joined from the Link menu */
    if (thread_on) {
      pthread_mutex_unlock(&core_mx);
      __real_plat_sleep_ms(ms > 0 ? ms : 1);
      pthread_mutex_lock(&core_mx);
      return;
    }
  }
  __real_plat_sleep_ms(ms);
}
