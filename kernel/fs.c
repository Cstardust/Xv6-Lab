// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
//  遍历bitmap , 通过bread获取free block 的 buf, 并返回获取的block的number
//  (bread)之后 该 buf就留在buf list中了，之后可以通过bread(blockno)进行访问
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  //  遍历所有bitmap block
  for(b = 0; b < sb.size; b += BPB){      //  b: 从第一个bitmap开始，已经走到第几个bit了
    // printf("bitmap %d , sb.size = %d , BPB = %d\n",b, sb.size, BPB);
    //  在disk上找到第 b/8192 个bitmap block  
    bp = bread(dev, BBLOCK(b, sb));
    //  遍历一个bitmap的所有bit(8192 bit)
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){  //  bi : 该bitmap中的第几个bit
      m = 1 << (bi % 8);
      //  找到为0的bit,即目标block,在bitmap buf上标记为可用 并
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  //  在buf上修改bitmap     Mark block in use. 
        log_write(bp);        //  在日志中记录修改bitmap
        brelse(bp);           //  释放对bp的使用
        bzero(dev, b + bi);   //  清空该bit对应的block 并日志记录 
        return b + bi;        //  返回获取的block的numeber
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block. 这个释放是说将该disk block清0吗 ? 不是，而是说这个disk block可以被覆盖了，里面的数据没用了。
//  清空在bitmap上对应的bit , 将block b的buf写入磁盘 , 释放buf
//  写入的这个buf全是0 ? 不然怎么做到释放disk block ? 
//  我想并没有将buf清0，而是仅仅通过将bitmap的相应bit清0来代表该block里面的数据已经没用，可以被覆盖，也即该block是一个free block ，可以再被分配。
//  作用一言以蔽之 ：释放disk上的真的一个block
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;
  //  block b buf
  bp = bread(dev, BBLOCK(b, sb));
  //  b在bitmap中位于第几个bit
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  //  清空该bit
  bp->data[bi/8] &= ~m;
  //  因需释放该block b buf，故应遵守原则，将该block buf b 写入日志
  log_write(bp);
  //  释放buf
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;       //  1. 保证同一个dinode在inode中最多出现一次。 2.  维护inode.ref的正确性。
  struct inode inode[NINODE];
} icache;

void
iinit()
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by giving it type type.
// Returns an unlocked but allocated and referenced inode.
//  从icache.inode数组中返回一个free的inode(已被type初始化)
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    //  获取disk上第inum个inode所属的block buf(位于buf list)
    bp = bread(dev, IBLOCK(inum, sb));      //  struct buf *bp : 一个block的cache。大小为1024bytes。包含多个inode
    //  获取该dinode
    dip = (struct dinode*)bp->data + inum%IPB;    // On-disk inode structure
    //  得到在block bp上的一个空闲的inode dip
    if(dip->type == 0){  // a free inode
      // printf("=============create file ialloc===========\n");
      memset(dip, 0, sizeof(*dip));
      //  通过将新type写入磁盘来声明它
      dip->type = type;
      // printf("dip->type = %d\n",dip->type);
      //  上述动作更新了block cache。因此需要通过log_write记录下来。
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
      //  在 struct inode[]中 分配一个插槽用于inum的inode     
      //  iget也是要找一个inode。iget找到的inode和上面的dip有啥区别？
      //  且这个返回的inode并不是在buf list中。和上面的buf中的dinode不同？
      //  这个iget获得的inode的type 还并没有被声明？但是上面inode buf的type被声明了已经？ 
      //  目前来看确实是这样。。为什么可以这样做？
    // in-memory copy of an inode
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  //  Copy a modified in-memory inode to disk
  //  将ip的元数据 写到 buf中的dinode 再写到disk上。
  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  
  //  inode cache ---copy into--> dinode buf
  //  也可说是
  //  inode 本身 --copy into--> dinode buf  
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
//  返回内存中的编号为inum的inode（不是在buf list中的，而是在icache.inode[]中）
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    //  找到inum对应的inode
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    //  inode数组中找到一个没用的插槽用于放inode
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  //  Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  //  初始化inode成为inum的inode
  ip = empty;
  ip->dev = dev;
  ip->inum = inum;  //  dev and inum
  ip->ref = 1;      //  引用为1
  ip->valid = 0;
  
  
  release(&icache.lock);
  //  返回该inode  
  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  //  可能会等待iput
  acquiresleep(&ip->lock);
  
  //  防止已经被iput删除
    //  如果已经被iput删除,则重新读取。
  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  // printf("ip = %p %d\n",ip,ip->ref);
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

//  放下对于inode的引用
// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquire(&icache.lock);

  //  ref == 1  即当前内核中除了本thread中的ip，没有人再引用该inode
  //  link == 0 即当前文件结构中 (磁盘上真实的物理结构中) 并没有(路径/文件)引用该inode
  //  则该inode已经无用，大小置0并释放其所引用的块 + 标记为未分配 + 写入磁盘
  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);
    release(&icache.lock);

    //  大小置0并释放其所引用的块
    itrunc(ip);
    //  type = 0 标记为未分配
    ip->type = 0;
    //  写入磁盘
    iupdate(ip);
    ip->valid = 0; //  属于inode但非dinode 不必写入磁盘

    releasesleep(&ip->lock);

    acquire(&icache.lock);
  }

  ip->ref--;  
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);        //  解锁
  iput(ip);           //  释放本thread对ip的引用
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// 返回inode的第bn个数据块的磁盘块号blockno。
// static uint
// bmap(struct inode *ip, uint bn)
// {
//   uint addr, *a;
//   struct buf *bp;
//   //  我愿将addr称为inode的插槽
//   //  如果inode的第bn个插槽 还并没有确定引用哪个 block，且是一个直接块插槽
//     //  那么，通过balloc，为其分配随意一个free block。在插槽addr中记录
//   if(bn < NDIRECT){
//     if((addr = ip->addrs[bn]) == 0)
//       ip->addrs[bn] = addr = balloc(ip->dev);
//     return addr;
//   }
//   bn -= NDIRECT;

//   if(bn < NINDIRECT){
//     // Load indirect block, allocating if necessary.
//     //  如果indirect间接块为0 ，那么为其分配balloc一个freeblock作为indirect block
//     if((addr = ip->addrs[NDIRECT]) == 0)
//       ip->addrs[NDIRECT] = addr = balloc(ip->dev);
//     //  获取indirect block buf
//     bp = bread(ip->dev, addr);
//     //  得到indirect block
//     a = (uint*)bp->data;
//     //  如果第bn项并没有指向一个block，则为其balloc分配一个freeblock，并记录
//     if((addr = a[bn]) == 0){
//       a[bn] = addr = balloc(ip->dev);
//       //  记入日志
//       log_write(bp);
//     }
//     //  使用完buf之后记得释放
//     brelse(bp);
//     return addr;
//   }
// // 如果块号超过NDIRECT+NINDIRECT，则bmap调用panic崩溃
//   panic("bmap: out of range");
// }




static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  //  1. NDIRECT = 11 
  //  11 个 direct block
  //  如果inode的bn为直接地址块，且还没引用datablock
  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  //  2. NINDIRECT = 256
  //  1 个 single indirect block
  if(bn < NINDIRECT){
    //  Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    //  get indirect block cache
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    //  如果第bn项并没有指向一个block，则为其balloc分配一个freeblock，并记录
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);   //  bread brelse 搭配
    return addr;
  }

  bn -= NINDIRECT;
  //  3. DOUBLE_NINDIRECT = 256 * 256
  //  1 个 double indirect block
  if(bn < DOUBLE_NINDIRECT)
  {
    //  addr pointed to the first level indirect block 
    if((addr = ip->addrs[NDIRECT+NINDIRECT]) == 0)
    {
      ip->addrs[NDIRECT+NINDIRECT] = addr = balloc(ip->dev);
    }
    bp = bread(ip->dev,addr);
    a = (uint*)bp->data;
    int n = bn / NINDIRECT;
    int m = bn % NINDIRECT;

    //  the first level indirect block pointed to the second level indirect block
    if((addr = a[n]) == 0)
    {
      a[n] = addr = balloc(ip->dev);  //  modify cache
      log_write(bp);
    }
    brelse(bp);

    //  the second level indirect block pointed to the target data block
    struct buf *m_bp = bread(ip->dev,addr);
    uint *m_a = (uint*)m_bp->data;
    if((addr = m_a[m]) == 0)
    {
      m_a[m] = addr = balloc(ip->dev);
      log_write(m_bp);
    }
    brelse(m_bp);
    return addr;
  } 

// 如果块号超过NDIRECT+NINDIRECT，则bmap调用panic崩溃
  panic("bmap: out of range");
}




// Truncate inode (discard contents).
// Caller must hold ip->lock.
//  清空inode : 释放inode所引用的块，并置inode代表的文件大小为0
  //  1. 释放其所指向的所有直接块(disk 上)
  //  2. 释放其所指向的所有间接块(disk 上)
  //  3. 文件大小置为0
// void
// itrunc(struct inode *ip)
// {
//   int i, j;
//   struct buf *bp;
//   uint *a;

//   //  释放所有直接引用的disk上的块 并清空addrs
//   for(i = 0; i < NDIRECT; i++){
//     if(ip->addrs[i]){
//       bfree(ip->dev, ip->addrs[i]);
//       ip->addrs[i] = 0;
//     }
//   }

//   //  释放所有间接引用的disk上的块 并清空addrs
//   if(ip->addrs[NDIRECT]){
//     bp = bread(ip->dev, ip->addrs[NDIRECT]);
//     a = (uint*)bp->data;
//     for(j = 0; j < NINDIRECT; j++){
//       if(a[j])
//         bfree(ip->dev, a[j]);
//     }
//     brelse(bp);
//     bfree(ip->dev, ip->addrs[NDIRECT]);
//     ip->addrs[NDIRECT] = 0;
//   }

//   //  inode代表的文件大小为0
//   ip->size = 0;
//   //  更新inode之后 应当立刻落入磁盘记入日志
//   iupdate(ip);
// }

//  blockno是一块direct address blockno
//  其中所有的数据都是指向datablock。也即都是datablockno
//  walkfreeblock : 存储在释放blockno中引用的所有datablock
static void walkfreeblock(int indirect_blockno,struct inode *ip)
{
  struct buf *bp = bread(ip->dev,indirect_blockno);
  uint* pd = (uint*)bp->data;   //  pointer to datablock
  for(int k=0;k<NINDIRECT;++k)
  {
    if(pd[k])
      bfree(ip->dev,pd[k]);
  }
  brelse(bp);                       //  不要忘记释放自身cache
  bfree(ip->dev,indirect_blockno);  //  不要忘记释放自身
}

void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  //  释放所有直接引用的disk上的块 并清空addrs
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  //  释放所有间接引用的disk上的块 并清空addrs
  if(ip->addrs[NDIRECT]){
    walkfreeblock(ip->addrs[NDIRECT],ip);
    ip->addrs[NDIRECT] = 0;
  }

  //  释放double indirect block 及 其所引用的block
  if(ip->addrs[NDIRECT + NINDIRECT])
  {
    bp = bread(ip->dev,ip->addrs[NDIRECT + NINDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j<NINDIRECT;++j)
    {
      if(a[j])
        walkfreeblock(a[j],ip);
    }
    brelse(bp);
    //  释放二级块表本身
    bfree(ip->dev,ip->addrs[NDIRECT + NINDIRECT]);
    ip->addrs[NDIRECT + NINDIRECT] = 0;
  }
  //  inode代表的文件大小为0
  ip->size = 0;
  //  更新inode之后 应当立刻落入磁盘记入日志
  iupdate(ip);
}



// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
//  读取从inode代表的文件的第off个bytes开始(从disk / buf)，读取n个bytes到dst中
//  disk ----datablock---> kernel buf list ---datablock buf---> user/kernel virtual adddress
//  off : 从file的第几个byte开始往外读。
//  dst : user/kernel virtual address
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  //  确保起始指针偏移量不超过文件的末尾。
  //  开始于超过文件末尾的地方读取将返回错误
  if(off > ip->size || off + n < off)
    return 0;
  //  确保最多只能读取到文件末尾
  if(off + n > ip->size)
    n = ip->size - off;
  //  将data从disk copy到内核内存再copy到用户内存
    //  total:总共读了file的多少bytes ; off:读到file的第几个byte了 ; dst : user virtual address
  for(tot=0 ; tot<n ; tot+=m, off+=m, dst+=m){
    //  block buf inode pointed to
    //  读取文件block内容（也即先从disk将文件的block内容取到内核中的buf list）
      //  bmap(ip, off/BSIZE) : 获取file的第off个bytes对应的block no
      //  bread(disk , blockno) : 获取blockno的buf
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    //  n-tot : 调用者希望读多少 ; BSIZE-off%BISZE : 这个块还能读多少 off%BSIZE应该是是在第一次有效。因为后面的off一定都是BSIZE的整数倍.
    m = min(n - tot, BSIZE - off%BSIZE);
    //  将buf拷贝到用户内存user_dst
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
//  用户从inode指向的第off个bytes开始，写n个bytes，从src address到buf到disk
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;
  //  起始指针off可以<=文件末尾
  if(off > ip->size || off + n < off)
    return -1;
  //  不得超过inode所能指向的块之和的最大字节数
  if(off + n > MAXFILE*BSIZE)
    return -1;
  //  tot : 当前写了多少bytes
  //  n : 总共要写多少bytes
  //  off : 当前写的是file中的第几个byte
  //  m : 当前轮写了多少byte
  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    //  disk -> kernel buf list
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    // printf("tot %d n %d off %d m %d bp->blockno %d\n",tot,n,off,m,bp->blockno);
    //  user virtual address -> kernel buf list
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    //  全部写入buf后，记入日志
    log_write(bp);
    brelse(bp);
  }

  //  若写操作扩展了文件，则更新inode.size
  if(off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  //  iupdate(ip) 因为在bmap时可能更新了inode.addr[]
  iupdate(ip);

  return tot;
}

// Directories
int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}


//  函数dirlookup（kernel/fs.c:527）在目录directory中搜索具有给定名称的条目entry。
//  如果找到
  //  1. 将*poff设置为条目在目录中的字节偏移量
  //  2. 通过iget获得的未锁定的inode 并 return inode*
//  如果没找到
  //  return 0
//  Dirlookup是iget返回未锁定indoe的原因
// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");
  //  从directory(inode指向的block)中读取data , directory的data就是struct dirent
  for(off = 0; off < dp->size; off += sizeof(de)){
    //  每轮读出一个entry : struct dirent
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    // printf("de->name = %s , name = %s\n",de.name,name);
    //  当前entry 是否代表 名为name的file
    //  当前的entry所能索引到的inode 是不是传入的name的inode 
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      // *poff设置为目录directory中条目entry的字节偏移量，以满足调用方希望对其进行编辑的情形
      if(poff)
        *poff = off;
      inum = de.inum;
      //  返回该entry所对应的inode
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
//  在directory dp下 ，新增一个entry。entry的name是xxx，指向的inode是123
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  //  如果本dpdirectory下已经有了名为name的entry 那么失败
  //  Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  //  找到空闲的entry struct dirent
  //  Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  //  写入name和inum
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  // printf("test\n");
  //  写入disk
    //  (如果现有的size里面没有free entry 那么就会增添新entry , 扩展dp->size)
  //  write 70 + write 32
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) 
    panic("dirlink");

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//  skipelem 将path中的 第一个path element(应该是个文件夹名字)取出拷贝到name中
//  返回path中出去path element之后剩余的内容
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
//  nameiparent = 0 : 返回path最后一个element(file/directory)的inode，并将最后一个element的名字拷贝到name中
//  nameiparent = 1 : 返回path最后一个element(file/directory)的的parent的inode，并将最后一个element的名字拷贝到name中
//  从哪里查找的？利用传入的name，去匹配(在directory inode指向的datablock中的所存储的)entry，得到inode的inum。
//  将path一级级，逐级拆分出一个个name，在当前directory inode下查找出(name对应entry，进而iget出)inode；
//  然后将该inode，作为下一级name的directory node，也即在这个刚查出来的inode下，查找name对应的inode。
//  一级级查找，直到查到path的最后一级别的inode。返回即可。
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  //  根目录开始查询 如/a/b/c
  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);  //  获取根目录文件夹的inode
  else
  //  当前目录开始查询 如 a/b/c
    ip = idup(myproc()->cwd);     //  获取当前目录文件夹的inode
  
  // printf("path = %s , name = %s\n",path,name);
  while((path = skipelem(path, name)) != 0){
    // printf("path = %s , name = %s\n",path,name);
    //  分别对路径名中的每个目录上锁，再调用dirlookup查找该目录。
    //  因此，就算有多个内核线程调用namex，只要dirlookup查找的目录是不同的，我们就可以并发地进行路径名查找。
    ilock(ip);
    //  inode不是directory 就查询失败 ? 
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    //  *path = 0 ，即path已经遍历完，现在name中保存的是path的最后一个element
    //  nameiparent = 1 ，即找到父节点即可。
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      //  当前inode(ip)就是path最后一个element的parent
      //  最后一个元素的名称在skipelem中已被复制到name
      return ip;
    }
    //  在directory inode下 根据我们之前拆解出的name (next=)找到name对应的inode 
    if((next = dirlookup(ip, name, 0)) == 0){
      //  ip下找不到name
      iunlockput(ip);
      return 0;
    }
    //  deadlock风险
    // 释放ip，为下一次循环做准备
    // (当查找'.'时，next和ip相同，若在释放ip的锁之前给next上锁就会发生deadlock
    //  因此namex在下一个循环中获取next的锁之前，要先释放本轮ip的锁，从而避免deadlock)
    //  侧面印证了：iget和ilock之间的分离很重要
    iunlockput(ip);
    //  下一级的inode
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  // nameiparent = 0 会走到这里
  return ip;
}

//  返回path最后一个element的inode
struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
