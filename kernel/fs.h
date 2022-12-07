// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block  bitmap的起始地址?
};

#define FSMAGIC 0x10203040

#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))  //  256
#define DOUBLE_NINDIRECT ((BSIZE / sizeof(uint))*(BSIZE / sizeof(uint)))
#define MAXFILE (NDIRECT + NINDIRECT + DOUBLE_NINDIRECT)

// On-disk inode structure
struct dinode {
  short type;           // File type  文件 / 目录 / 特殊文件 / 0表示inode空闲
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+2];   // Data block addresses 保存inode代表的文件的内容的磁盘块号
// 11 * direct block + 1 single indirect block + 1 * double indirect block
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))   //  IPB = 1024 / sizeof(struct dinode)

// Block containing inode i
//  找到第i个inode所属的inode block
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)   //  blocksize = 1024 ; * 8 = 8192 bits

// Block of free map containing bit for block b
// 找到第 b/8192 个bitmap block
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)
// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

//  文件夹中的一系列entry 而非 文件夹
struct dirent {
  ushort inum;    //  用于索引inode
  char name[DIRSIZ];
};

