#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

#define NBUCKET 5
#define NKEYS 100000


//  hash table k-v
struct entry
{
  int key;
  int value;
  struct entry *next;
};

struct entry *table[NBUCKET];
pthread_mutex_t locks[NBUCKET];   //  每个bucket1个lock

int keys[NKEYS];
int nthread = 1;

double
now()
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

//  这里需要lock。不然多个thread同时insert。会有被覆盖的。
static void
insert(int key, int value, struct entry **p, struct entry *n)
{
  struct entry *e = malloc(sizeof(struct entry));
  e->key = key;
  e->value = value;
  e->next = n;
  *p = e;
}

static void put(int key, int value)
{
  int i = key % NBUCKET;

  // is the key already present?
  struct entry *e = 0;
  pthread_mutex_lock(&locks[i]);
  for (e = table[i]; e != 0; e = e->next)
  {
    if (e->key == key)
      break;
  }
  if (e)
  {
    // update the existing key->value.
    e->value = value;
  }
  else
  {
    // the new is new.
    insert(key, value, &table[i], table[i]);
  }
  pthread_mutex_unlock(&locks[i]);
}

static struct entry *
get(int key)
{
  int i = key % NBUCKET;

  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next)
  {
    if (e->key == key)
      break;
  }

  return e;
}

//  每个thread平摊 NKEYS 100000 / threads 个 put key。应当可以整除，不整除的话 会漏掉余下的key没put
static void *
put_thread(void *xa)
{
  int n = (int)(long)xa; // thread number
  int b = NKEYS / nthread;

  for (int i = 0; i < b; i++)
  {
    put(keys[b * n + i], n);
  }

  return NULL;
}

//  get_thread func 用于 验证在multi-thread在put时 是否发生了 data race
//  multi-thread在运行get_thread自身不会发生data race
static void *
get_thread(void *xa)
{
  int n = (int)(long)xa; // thread number
  int missing = 0;

  //  查找所有应当插入了hash table的key
  //  如果有没有找到的，就意味着在multi-thread在插入时发生了data race
  for (int i = 0; i < NKEYS; i++)
  {
    struct entry *e = get(keys[i]);
    if (e == 0)
      missing++;
  }
  printf("%d: %d keys missing\n", n, missing);    //  不同thread都相同。符合。因为只读。
  return NULL;
}


void initlocks()
{
  for(int i=0;i<NBUCKET;++i)
  {
    // initialize the lock
    pthread_mutex_init(&locks[i], NULL); 
  }
}

int main(int argc, char *argv[])
{
  pthread_t *tha;     //  thread pid 数组
  void *value;
  double t1, t0;

  if (argc < 2)
  {
    fprintf(stderr, "Usage: %s nthreads\n", argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);
  assert(NKEYS % nthread == 0);
  for (int i = 0; i < NKEYS; i++)
  {
    keys[i] = random();
  }


  initlocks();

  //
  // first the puts
  //
  t0 = now();
  for (int i = 0; i < nthread; i++)
  {
    assert(pthread_create(&tha[i], NULL, put_thread, (void *)(long)i) == 0);
  }
  for (int i = 0; i < nthread; i++)
  {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d puts, %.3f seconds, %.0f puts/second\n",
         NKEYS, t1 - t0, NKEYS / (t1 - t0));

  //
  // now the gets
  //
  t0 = now();
  for (int i = 0; i < nthread; i++)
  {
    assert(pthread_create(&tha[i], NULL, get_thread, (void *)(long)i) == 0);
  }
  for (int i = 0; i < nthread; i++)
  {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d gets, %.3f seconds, %.0f gets/second\n",
         NKEYS * nthread, t1 - t0, (NKEYS * nthread) / (t1 - t0));
}