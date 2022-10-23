// struct buf {
//   int valid;   // has data been read from disk?
//   int disk;    // does disk "own" buf?
//   uint dev;
//   uint blockno;
//   //  防止多个process同时对一个cache进行读写操作
//   struct sleeplock lock;  //  sleep lock for this buf
//   uint refcnt;
//   struct buf *prev;       
//   struct buf *next;       // cache list 双向不循环链表
//   uchar data[BSIZE];

//   uint timestamp;
//   int bucket_idx;
// };



struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
  uint timestamp; // 上一次使用该buf的时间
  int bucket_idx;
};

