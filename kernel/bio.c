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
        release(&bcache.evict_race_lock);                     //  不要和下一句写反!!
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
      continue;     //  可能全部都是refcnt!=0
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




// // Buffer cache.
// //
// // The buffer cache is a linked list of buf structures holding
// // cached copies of disk block contents.  Caching disk blocks
// // in memory reduces the number of disk reads and also provides
// // a synchronization point for disk blocks used by multiple processes.
// //
// // Interface:
// // * To get a buffer for a particular disk block, call bread.
// // * After changing buffer data, call bwrite to write it to disk.
// // * When done with the buffer, call brelse.
// // * Do not use the buffer after calling brelse.
// // * Only one process at a time can use a buffer,
// //     so do not keep them longer than necessary.


// #include "types.h"
// #include "param.h"
// #include "spinlock.h"
// #include "sleeplock.h"
// #include "riscv.h"
// #include "defs.h"
// #include "fs.h"
// #include "buf.h"

// #define NBUCKETS 17
// #define HASHMAP(k) k%NBUCKETS

// char bcachename[NBUCKETS][20];

// struct {
//   struct spinlock lock[NBUCKETS]; // 每个HASH桶对应一把锁
//   struct spinlock loaddisklock; // 从disk加载块到buf的串行锁
//   struct buf buf[NBUF];
//   struct buf hashbucket[NBUCKETS]; // 全部HASH桶

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   // struct buf head;

// } bcache;

// void
// binit(void)
// {
//   struct buf *b;

//   // 初始化全部的桶和锁
//   for(int i=0; i<NBUCKETS; i++){
//     snprintf(bcachename[i], sizeof(bcachename[i]), "bcache_hashbucket%d", i);
//     initlock(&bcache.lock[i], bcachename[i]);
//     bcache.hashbucket[i].prev = &bcache.hashbucket[i];
//     bcache.hashbucket[i].next = &bcache.hashbucket[i]; 
//   }

//   // Create linked list of buffers
//   int count=0;
//   // 初始化全部buf
//   // 并用头插法均匀挂在每隔HASH桶下
//   for(b = bcache.buf; b < bcache.buf+NBUF; b++){
//     b->timestamp = 0;
//     b->refcnt = 0;
//     b->next = bcache.hashbucket[HASHMAP(count)].next;
//     b->prev = &bcache.hashbucket[HASHMAP(count)];
//     initsleeplock(&b->lock, "buffer");
//     bcache.hashbucket[HASHMAP(count)].next->prev = b;
//     bcache.hashbucket[HASHMAP(count)].next = b;
//     count++;
//   }

//   initlock(&bcache.loaddisklock, "bcache_loaddisk");
// }


// // Look through buffer cache for block on device dev.
// // If not found, allocate a buffer.
// // In either case, return locked buffer.
// static struct buf*
// bget(uint dev, uint blockno)
// {
//   // key -> HASHMAP(blockno)
//   struct buf *b;

//   // 获取块号对应桶的锁
//   acquire(&bcache.lock[HASHMAP(blockno)]);

//   /* 1.遍历自身的桶 看能否cached */

//   // 是一个双向循环链表
//   // Is the block already cached?
//   for(b = bcache.hashbucket[HASHMAP(blockno)].next; b != &bcache.hashbucket[HASHMAP(blockno)]; b = b->next){
//     if(b->dev == dev && b->blockno == blockno){
//       b->refcnt++;
//       release(&bcache.lock[HASHMAP(blockno)]); // 找到cache了，可以释放桶锁
//       acquiresleep(&b->lock); // 但要获取buf锁并返回
//       return b;
//     }
//   }

//   /* 2.没有cached 需要找一个LRU的buf */

//   // Not cached.
//   // Recycle the least recently used (LRU) unused buffer.   

//   // 先释放当前桶锁
//   release(&bcache.lock[HASHMAP(blockno)]);

//   // 获得装载锁
//   // 保证cpu串行进入载入流程
//   acquire(&bcache.loaddisklock);

//   // cpu串行进入载入流程的时候，再次检查是否命中，已经有了就不载入了，防止载入多次
//   // 获取当前桶的锁
//   acquire(&bcache.lock[HASHMAP(blockno)]);
//   for(b = bcache.hashbucket[HASHMAP(blockno)].next; b != &bcache.hashbucket[HASHMAP(blockno)]; b = b->next){
//     if(b->dev == dev && b->blockno == blockno){
//       b->refcnt++;
//       release(&bcache.lock[HASHMAP(blockno)]); // 找到cache了，可以释放当前桶的锁
//       release(&bcache.loaddisklock);
//       acquiresleep(&b->lock); // 但要获取buf锁并返回
//       return b;
//     }
//   }

//   // 真的没有命中，选择LRU的buf，加入当前桶中
//   struct buf *tempbuf; // temp buf
//   b = 0;

//   // 开始全局遍历，遍历所有的桶，从当前桶开始
//   for(int i = 0; i < NBUCKETS; i++){
//     int hashbucketid = HASHMAP((i+HASHMAP(blockno))); // 遍历桶，从当前桶开始遍历
//     if(hashbucketid!=HASHMAP(blockno))
//       // 获取遍历桶的锁
//       acquire(&bcache.lock[hashbucketid]);
//     // 遍历该桶
//     for(tempbuf = bcache.hashbucket[hashbucketid].next; tempbuf != &bcache.hashbucket[hashbucketid]; tempbuf = tempbuf->next){
//       // 若b为空，则初始化一个b
//       // 找到最新的LRU buf，更新b
//       if(tempbuf->refcnt==0 && (b ==0 || tempbuf->timestamp < b->timestamp))
//         b = tempbuf;
//     }

//     // 遍历桶完成，找到一个有效的b，处理并返回
//     if(b){
//       // 如果不是自己的桶中，则取出并插入当前桶中
//       if(hashbucketid!=HASHMAP(blockno)){
//         // 遍历桶取出b
//         b->next->prev = b->prev;
//         b->prev->next = b->next;
//         // 释放遍历桶的锁
//         release(&bcache.lock[hashbucketid]);

//         // 加入当前桶
//         b->prev = &bcache.hashbucket[HASHMAP(blockno)];
//         b->next = bcache.hashbucket[HASHMAP(blockno)].next;
//         bcache.hashbucket[HASHMAP(blockno)].next->prev = b;
//         bcache.hashbucket[HASHMAP(blockno)].next = b;

//       }

//       // 现在b在自己的桶中
//       // 初始化b
//       b->dev = dev;
//       b->blockno = blockno;
//       b->valid = 0;
//       b->refcnt = 1;

//       // 释放当前桶的锁，获取buf锁，释放载入锁，并返回b
//       release(&bcache.lock[HASHMAP(blockno)]);
//       release(&bcache.loaddisklock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//     // 否则继续下一个遍历桶
//     else{
//       if(hashbucketid!=HASHMAP(blockno))
//         // 释放遍历桶的锁
//         release(&bcache.lock[hashbucketid]);
//     }
//   }



//   // 走到这说明了发生错误
//   // 释放当前桶的锁，释放载入锁
//   release(&bcache.lock[HASHMAP(blockno)]);
//   release(&bcache.loaddisklock);
//   panic("bget: no buffers");
// }

// // Return a locked buf with the contents of the indicated block.
// struct buf*
// bread(uint dev, uint blockno)
// {
//   struct buf *b;

//   b = bget(dev, blockno);
//   if(!b->valid) {
//     virtio_disk_rw(b, 0);
//     b->valid = 1;
//   }
//   return b;
// }

// // Write b's contents to disk.  Must be locked.
// void
// bwrite(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("bwrite");
//   virtio_disk_rw(b, 1);
// }

// // Release a locked buffer.
// // Move to the head of the most-recently-used list.
// void
// brelse(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("brelse");

//   releasesleep(&b->lock);

//   acquire(&bcache.lock[HASHMAP(b->blockno)]);
//   b->refcnt--;
//   if (b->refcnt == 0) {
//     // no one is waiting for it.
//     // 仅在buf块没有引用的时候再更新时间戳
//     // why? -> 时间戳是为了bget()在refcnt为0的buf块中选择而用的
//     b->timestamp = ticks;
//   }
  
//   release(&bcache.lock[HASHMAP(b->blockno)]);
// }

// void
// bpin(struct buf *b) {
//   // 获取b->blockno对应的HASH桶的锁
//   acquire(&bcache.lock[HASHMAP(b->blockno)]);
//   b->refcnt++;
//   release(&bcache.lock[HASHMAP(b->blockno)]);
// }

// void
// bunpin(struct buf *b) {
//   acquire(&bcache.lock[HASHMAP(b->blockno)]);
//   b->refcnt--;
//   release(&bcache.lock[HASHMAP(b->blockno)]);
// }
