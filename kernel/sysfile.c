//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}


// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
//  为struct file 分配一个free的fd
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

static struct inode* create(char *path, short type, short major, short minor);

uint64 sys_symlink(void)
{
  char target[MAXPATH] = {0};
  char path[MAXPATH] = {0};
  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;


  begin_op();

  //  1. 创建一个symbolic inode
  struct inode* ip = namei(path); 
  if(ip==0)   
  {
    //  如果之前没有，创建inode
    ip = create(path,T_SYMLINK,0,0);
    if(ip == 0)
    {
      end_op();
      return -1;
    }
    itrunc(ip);                     //  清空inode的引用
  }
  else
  //  如果之前就有,那么写覆盖datablock即可
  {
    ilock(ip);
  }

  //  2. 向该symbolic inode的datablock写入数据:char *target。当open的时候访问该target路径对应的inode 
  int ret = writei(ip,0,(uint64)target,0,MAXPATH);
  if(ret != MAXPATH) 
  {
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  // iunlock(ip); wrong panic
  end_op();
  return 0;
}



uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  // printf(".......\n");
  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  //  old : 老文件的name
  //  new : 即将新建立的entry的name
  //  目的 : 要在new所在的directory,建立一个新entry(old_inum , new_entry_name)
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  //   new 是一个新的 要指向old inode 的entry的name 
  //   这个new不是一个file的name，而仅仅是一个entry的name。entry不是file，并不需要由inode代表，而仅仅是一个directory的inode的datablock中的数据
  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  //  根据获取old对应的inode
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }
  //  固定inode
  ilock(ip);
  //   hard link not allowed for directory 
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  // inode.nlink++
  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  //  找到new的parent inode ：dp
  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  //  directory dp下 新增entry(inum,name)
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

//  create 为新inode创建一个新名称的新entry
//  返回一个被锁定的inode
//  什么叫做被锁定：即持有inode的锁。即在create中ilock(ip),但却并没有释放
static struct inode*
create(char *path, short type, short major, short minor)
{

  struct inode *ip, *dp;
  char name[DIRSIZ];

  //  找到path 最后一个 element的 parent directory
  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  //  在dp下，获取entry(inum , name) 对应的inode
  if((ip = dirlookup(dp, name, 0)) != 0){
    //  如果该name对应的entry对应的inode
    iunlockput(dp);
    ilock(ip);
    //  如果type == T_FILE(即create代表open) inode本身确实代表T_FILE / T_DEVIEC 则视为sucess。return inode
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    //  否则视为fail
    return 0;
  }

  //  如果该name对应的entry对应的inode并不存在，那么进行分配inode
  //  获取初始化了type的inode 
    //  inode.type = type , 将inode标记为被使用
  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  //  inode其他属性初始化 , 如inode.link
  //  在读取或写入inode的元数据或内容之前，代码必须使用ilock锁定inode。
  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  //  硬链接计数1
  ip->nlink = 1;

  // printf("=============create file iupdate===========\n");
  iupdate(ip);

  //  如果create是要创建一个目录 , 也即 inode要作为一个目录的inode
  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    //  在新建dp下，新增entry(.)(..)
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  //  正确初始化新inode之后，将其链接到parent inode(dp)下
  if(dirlink(dp, name, ip->inum) < 0)   //  write 70
    panic("create: dirlink");


  iunlockput(dp);
  //  Create与sys_link一样，同时持有两个inode锁：ip和dp。
  //  不存在死锁的可能性，因为索引结点ip是新分配的：系统中没有其他进程会持有ip的锁，然后尝试锁定dp。?
  
  return ip;
}

//  fail : return 0
//  success : return locked inode*
static struct inode* dfs_trace_symlink(struct inode *ip,int h)
{
  //  如果超出最大深度,则返回0
  //  如果是要target路径的inode不存在，则返回0
  if(h > 10 || ip == 0) {return 0;}

  // 在读写inode的元数据或内容之前，代码必须使用ilock锁定inode
  ilock(ip);

  //  如果本inode是个软链接inode，那么一路追踪到非软链接inode
  if(ip->type != T_SYMLINK) return ip; 
  char target[MAXPATH] = {0};
  int ret = readi(ip,0,(uint64)target,0,MAXPATH);
  if(ret != MAXPATH) panic("open symlink");

  iunlockput(ip);

  struct inode* ne = namei(target);
  return dfs_trace_symlink(ne,h+1);
}

//  创建文件
uint64
sys_open(void)
{
  // printf("=============create file start===========\n");

  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  //  如果open被传递了O_CREATE标志，它将调用create（kernel/sysfile.c:301）。
  //  create ilock(inode)
  if(omode & O_CREATE){
    //  write 33inodeblock ; 33inodeblock ; 70parentdatablock ; 32parentinodeblock
    ip = create(path, T_FILE, 0, 0);    
    if(ip == 0){
      end_op();
      return -1;
    }
  //  否则，它将调用namei。打开已有文件
  //  namei不ilock(inode)
  } else {
    //  open("/testsymlink/b", O_RDWR)
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }

    //  trace symlink . 
    //  失败则返回NULL ,syscall return -1。
    //  成功则获得目标inode指针 , syscall return fd。
    if(ip->type == T_SYMLINK && ((omode & O_NOFOLLOW) == 0))
    {
      ip = dfs_trace_symlink(ip,0);
      if(ip == 0) 
      {
        end_op();
        return -1;
      }
    }
    else
    {
      ilock(ip);
    }

    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  //  从ftable中分配free struct file 来承载inode
  //  从p->ofile中分配free fd 来索引struct file
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }


  //  inode挂在struct file上
  f->ip = ip;
  //  权限
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    //  write 33 newfileinode
    itrunc(ip);
  }

  // printf("=============create file end===========\n");

  iunlock(ip);
  end_op();


  return fd;    
  //  process通过在struct proc中的struct file *ofile[NOFILE] 查询fd来索引struct file
  //  struct file中有inode的指针,inode即文件本身（元信息）
  //  inode中有addrs，指向存储其数据的datablock
}




uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

