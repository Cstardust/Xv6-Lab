struct buf {
  int valid;   // buf是否包含block副本  has data been read from disk?
  int disk;    // buf内容是否已经交给磁盘 does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;    //  保护buf cache自身 ？ 还是保护什么？这个buf是inode吗？
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};

