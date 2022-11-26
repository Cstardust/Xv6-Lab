#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

static int nthread = 1;
static int round = 0;

struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  int nthread;      // Number of threads that have reached this round of the barrier
  int round;     // Barrier round
  //  上一轮的thread 是否都已经从barrier中离开
  pthread_cond_t finished_cond;
  int finished;
} bstate;

static void
barrier_init(void)
{
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
  bstate.finished = 1;
}

static void barrier()
{
  pthread_mutex_lock(&bstate.barrier_mutex);
  //  当上一轮结束了 再来启动下一轮的计数。  
  while(!(bstate.finished==1))
  {
    pthread_cond_wait(&bstate.finished_cond,&bstate.barrier_mutex);
  }
  //  确保上一轮thread都离开barrier之后，本轮thread真正进入barrier，并进行计数。
  ++bstate.nthread;
  //  是否会存在，本轮的thread还没全部离开，下一轮的thread就来了的情况？
    //  我认为显然是会出现的。
  if(bstate.nthread != nthread)
  {
    //  其他线程等待最后一个线程到达
    pthread_cond_wait(&bstate.barrier_cond,&bstate.barrier_mutex);
    --bstate.nthread;
    //  告诉下一轮来的thread 本轮结束，不必再等，可以进入barrier。
    if(bstate.nthread == 0)
    {
      bstate.finished = 1;            
      pthread_cond_broadcast(&bstate.finished_cond);      
    }
    pthread_mutex_unlock(&bstate.barrier_mutex);
    // pthread_cond_broadcast(&bstate.barrier_cond);
  }
  else
  {
  //  最后一个到达的线程
    //  告诉下一轮来的thread 本轮未结束，等着，不可进入barrier。
    bstate.finished = 0;            
    --bstate.nthread;
    ++bstate.round;
    //  告诉下一轮来的thread 本轮结束，不必再等，可以进入barrier。
    if(bstate.nthread == 0)
    {
      bstate.finished = 1;            
      pthread_cond_broadcast(&bstate.finished_cond);      
    }
    //  唤醒本轮的在barrier中等待的thread，告诉他们可以离开barrier。
    pthread_mutex_unlock(&bstate.barrier_mutex);
    pthread_cond_broadcast(&bstate.barrier_cond);
  }
}


static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;
    assert (i == t);
    barrier();
    usleep(random() % 100);
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}
