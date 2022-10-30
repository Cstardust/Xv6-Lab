// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUCKET_NUM 13
#define NULL (void*)0

static int hash(int key)
{
  return (key%BUCKET_NUM + BUCKET_NUM) % BUCKET_NUM;
}

typedef struct bucket{
  struct spinlock lock;
  struct buf head;
} bucket;

struct {
  struct spinlock evict_race_lock;
  struct buf buf[NBUF];
  struct bucket buckets[BUCKET_NUM];
} bcache;

static void Assert(int flag,char *msg)
{
  if(flag == 0){
    printf("Assert %s\n",msg);
    exit(-1);
  }
}


static void insertBufferInToBucket(struct buf *node,struct buf *head)
{
  node->prev = head;
  node->next = head->next;
  if(head->next)
    head->next->prev = node;
  head->next = node;
}


// static void showBucket(int idx)
// {
//   struct buf *cur = bcache.buckets[idx].head.next;
//   do
//   {
//     printf("cur = %p\n",cur);
//     cur = cur->next;
//   }while(cur);
// }

void
binit(void)
{
  // printf("============Start Binit============\n");
  initlock(&bcache.evict_race_lock,"evict_race_lock");

  //  init buckets
  for(int i=0;i<BUCKET_NUM;++i)
  {
    initlock(&bcache.buckets[i].lock,"bucket_lock");
    bcache.buckets[i].head.prev = bcache.buckets[i].head.next = NULL;
  }

  // init locks of all buffers
  for(int i=0;i<NBUF;++i)
  {
    initsleeplock(&bcache.buf[i].lock,"buffer_lock");
    insertBufferInToBucket(&bcache.buf[i],&bcache.buckets[0].head);
    bcache.buf[i].bucket_idx = 0;
  }
  // showBucket(0);
  // printf("============End Binit============\n");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  // printf("=============Start Get=================\n");
// Is the block already cached?
  int idx = hash(blockno);
  //  bucket lock because of reading / writing bucket list
  acquire(&bcache.buckets[idx].lock);
  struct buf * cur = bcache.buckets[idx].head.next;
  while(cur!=NULL)
  {
    if(cur->dev == dev && cur->blockno==blockno)
    {
      Assert(cur->bucket_idx == idx , "cur->bucket_idx == idx");
      ++cur->refcnt;
      release(&bcache.buckets[idx].lock);                  //  release bucket lock for ending modifying bucket
      acquiresleep(&cur->lock);                            //  lock for reading/writing buffer
      // printf("=============End Get==============\n");
      return cur;                                          //  return cached buffer
    }
    cur = cur->next;
  }

  release(&bcache.buckets[idx].lock);                       //  防止环路等待

  acquire(&bcache.evict_race_lock);                         //  防止两个process 同时为了 一块buffer 去驱逐其他buffer。因为已经释放了idx桶的锁，故可能有多个process进入驱逐代码段。
  {
    struct buf * cur = bcache.buckets[idx].head.next;
    while(cur!=NULL)
    {
      if(cur->dev == dev && cur->blockno==blockno)
      {
        Assert(cur->bucket_idx == idx , "cur->bucket_idx == idx");
        acquire(&bcache.buckets[idx].lock);
        ++cur->refcnt;
        release(&bcache.buckets[idx].lock);                  //  release bucket lock for ending modifying bucket
        release(&bcache.evict_race_lock);                     //  bug 不要和下一句写反!!
        acquiresleep(&cur->lock);                            //  lock for reading/writing buffer
        // printf("=============End Get==============\n");
        return cur;                                          //  return cached buffer
      }
      cur = cur->next;
    }
  }


// Not cached.
// Recycle the least recently used (LRU) unused buffer.
  struct buf *lru_buf = NULL;
  for(int i=0;i<BUCKET_NUM;++i)
  {
    acquire(&bcache.buckets[i].lock);
    struct buf *i_lru_buf = NULL;
    struct buf *pb = bcache.buckets[i].head.next;
    
    if(pb==NULL) {
      release(&bcache.buckets[i].lock);
      continue;      //  空链表 不可能有 跳过
    }
    
    while(pb!=NULL)
    {
      if(pb->refcnt!=0)         
      {
        pb = pb->next;
        continue;
      }
      if(i_lru_buf == NULL){
        i_lru_buf = pb;
        pb = pb->next;
        continue;
      }
      if(i_lru_buf->timestamp > pb->timestamp)
      {
        i_lru_buf = pb;
        pb = pb->next;
        continue;
      }
      pb = pb->next;
    }

    if(i_lru_buf == NULL) {
      release(&bcache.buckets[i].lock);
      continue;     //  bug 别忘记释放锁 可能全部都是refcnt!=0
    }
    //  update lru_buf
    if(lru_buf == NULL)
    {
      lru_buf = i_lru_buf;
      continue;
    }
    // printf("idx %d i %d i_lru_buf = %p , lru_buf = %p\n",idx,i,i_lru_buf,lru_buf);      
    Assert(i_lru_buf!=NULL,"i_lru_buf!=NULL");
    Assert(lru_buf!=NULL,"lru_buf!=NULL");

    //  update lru_buf release old lock
    if(lru_buf->timestamp > i_lru_buf->timestamp)
    {
      release(&bcache.buckets[lru_buf->bucket_idx].lock);
      lru_buf = i_lru_buf;
    }
    //  release i lock
    else
    {
      release(&bcache.buckets[i].lock);
    }
  }
  

  if(lru_buf == NULL)
  {
    panic("bget: no buffers");
  }

  int lru_buf_bucket_idx = lru_buf->bucket_idx;

  // acquire(&bcache.buckets[lru_buf_bucket_idx].lock);
  Assert(lru_buf->refcnt == 0 , "lru_buf->refcnt==0");
  lru_buf->dev = dev;
  lru_buf->blockno = blockno;
  lru_buf->valid = 0;
  lru_buf->refcnt = 1;
  lru_buf->bucket_idx = idx;
  //  evict : remove from the old list
  if(lru_buf->prev)     //  false at first time
  {
    lru_buf->prev->next = lru_buf->next;
  }
  if(lru_buf->next)
  {
    lru_buf->next->prev = lru_buf->prev;
  }
  release(&bcache.buckets[lru_buf_bucket_idx].lock);


  acquire(&bcache.buckets[idx].lock);
  //  append to the new list
  insertBufferInToBucket(lru_buf,&bcache.buckets[idx].head);
  release(&bcache.buckets[idx].lock);

  release(&bcache.evict_race_lock);
  
  acquiresleep(&lru_buf->lock);

  // printf("=============End Get==============\n");
  return lru_buf;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)     //  请求缓存了dev内容的blockno号磁盘块？
{
  struct buf *b;
  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);       //  如果未命中，将此磁盘块从磁盘加载进缓存
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  // printf("=============in brelse==========\n");
  if(!holdingsleep(&b->lock))
    panic("brelse");

  int idx = b->bucket_idx;
  Assert(idx == hash(b->blockno),"idx == hash(b->blockno)");
  acquire(&bcache.buckets[idx].lock);
  
  releasesleep(&b->lock);

  --b->refcnt;
  if(b->refcnt == 0)
  {
    b->timestamp = ticks;
  }
  release(&bcache.buckets[idx].lock);
  // printf("=============end brelse=============\n");
}

void
bpin(struct buf *b) {
  acquire(&bcache.buckets[b->bucket_idx].lock);
  b->refcnt++;
  release(&bcache.buckets[b->bucket_idx].lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.buckets[b->bucket_idx].lock);
  b->refcnt--;
  release(&bcache.buckets[b->bucket_idx].lock);
}


