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
#include "memlayout.h"
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
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
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

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
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
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
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




// [v->start , v->sz - 1]
// [v->start , v->sz )
uint64 getVmaEnd(struct vma *v)
{
  return v->start + v->sz;
}

int validMmap(uint64 addr,struct vma** vma)
{
  // printf("fault addr %p\n",addr);
  struct proc *p = myproc();
  for(int i=0;i<NVMA;++i)
  {
    if(p->vmas[i].valid == 1)
    {
      uint64 end = getVmaEnd(&(p->vmas[i]));
      uint64 start = p->vmas[i].start;
      // printf("start %p end %p sz %p\n",start,end,p->vmas[i].sz);

      if(addr >= start && addr < end)
      {
        *vma = &(p->vmas[i]);
        return 1;
      }
    }
  }
  return 0;
}



uint64
sys_mmap(void)
{
  // printf("=============sys_mmap start===============\n");
  uint64 addr;
  uint64 sz;
  int prot;
  int flag;
  int fd;
  uint64 offset;

  if(argaddr(0,&addr) < 0 || argaddr(1,&sz) < 0 || argint(2,&prot) < 0 || argint(3,&flag) < 0 || argint(4,&fd) < 0 || argaddr(5,&offset) < 0)
  {
    printf("failed to get the args\n");
    return -1;
  }


  struct proc *p = myproc();

  //  获取fd的file
  if(fd < 0 || fd >= NOFILE) panic("sys_mmap");
  struct file *f = p->ofile[fd];

  //  判定权限是否矛盾
    //  file不可读 user要求可读
  if( (!f->readable) && (prot & PROT_READ) ) 
  {
    printf("file can not read but you want to read\n");
    return -1;
  }
    //  file不可写 user要求写file
    //  f->writable : file可写
    //  PROT_WRITE : user要求写映射内存
    //  flag : user要求将写的内容落入磁盘
  if( (!f->writable) && ((prot & PROT_WRITE) && (flag & MAP_SHARED)) )
  {
    printf("file can not write but you want to write\n");
    return -1;
  }

  //  计算映射大小
  sz = PGROUNDUP(sz);
  uint64 mapuplimit = TRAPFRAME;
  //  顺序寻找可映射的vma槽 并 计算 本次映射的end地址。
  int idx = 0;
  int found = 0;
  for(int i = 0 ; i < NVMA ; ++i)
  {
    // printf("i = %d , ",i);
    if(p->vmas[i].valid == 0 && !found)
    {
      // printf("valid\n");
      p->vmas[i].valid = 1;
      idx = i;
      found = 1;
    }
    //  找到proc中最低的 可以提供给 mmap的 end虚拟地址
    else if(p->vmas[i].valid == 1 && mapuplimit > p->vmas[i].start)
    {
      // printf("mapuplimit %p , p->vmas[i].start %p\n",mapuplimit,p->vmas[i].start);
      mapuplimit = p->vmas[i].start;  //  一定是对PGSIZE对齐的。
    }
  }

  //  初始化vma
  p->vmas[idx].f = f;
  p->vmas[idx].sz = sz;
  p->vmas[idx].left = sz;
  p->vmas[idx].start = PGROUNDDOWN(mapuplimit - sz);  //  start一定是PGSIZE对齐的 sz可能不是
  p->vmas[idx].prot = prot;
  p->vmas[idx].flag = flag;
  p->vmas[idx].offset = offset;  
  filedup(f);     //  ++ref

  // printf("VMA mapuplimit %p start %p , end %p , sz %p , \n",mapuplimit,p->vmas[idx].start,getVmaEnd(&p->vmas[idx]),p->vmas[idx].sz);

  // printf("=============sys_mmap end===============\n");

  return p->vmas[idx].start;
}

// munmap(addr, length) should remove mmap mappings in the indicated address range. 
// If the process has modified the memory and has it mapped MAP_SHARED, the modifications should first be written to the file. 
// An munmap call might cover only a portion of an mmap-ed region,
// but you can assume that it will either unmap at the start, 
// or at the end, 
// or the whole region 
// (but not punch a hole in the middle of a region).
uint64 
sys_munmap(void)
{
  // printf("=============sys_munmap start===============\n");

  uint64 addr;
  uint64 sz;
  //  取参数
  if(argaddr(0,&addr) < 0 || argaddr(1,&sz) < 0)
  {
    printf("failed to get arg\n");
    return -1;
  }
  // printf("user input addr %p sz %p\n",addr,sz);

  struct vma *vma;
  //  检验地址合法性
  // you can assume that it will either unmap at the start, or at the end, or the whole region (but not punch a hole in the middle of a region).
  if(!validMmap(addr,&vma))
  {
    printf("unmmapped region\n");
    return -1;
  }
  if(addr < vma->start || addr > getVmaEnd(vma))
  {
    printf("beyond region\n");
    return -1;
  }
    //  not to dig a hole
  if(addr > vma->start && addr + sz < getVmaEnd(vma))
  {
    printf("forbiddened to dig a hole ?\n");
    return -1;
  }

  //  烦人的计算要munmap的范围
  //////////////////////////////////////////////////////////////

  //  只有munmap至少有一端在vma->start或者vma->end时 才是合法munmap的
    //  合法的释放范围只有这三种
      //  声明 vma->end = getEndVma(vma);
      //  [vma->start , x)   (x <= vma->end)  x不必和PGSIZE对齐
      //  [x , vma->end)     (x >= vma->start)      x不必和PGSIZE对齐
      //  [vma->start , vma->end)             whole
  uint64 start = PGROUNDDOWN(addr);   //  起始地址向下对齐

  if(start + sz >= getVmaEnd(vma) || start > vma->start)
  {
    sz = getVmaEnd(vma) - start;
  }

  // printf("I will really munmap from addr %p sz %p region end is %p\n",start,sz,getVmaEnd(vma));

  //////////////////////////////////////////////////////////////

  struct proc *p = myproc();
  pagetable_t pgtb = p->pagetable;
  //  真正的核心实现逻辑在这个函数里
  vmamunmap(vma,start,sz,pgtb);

  vma->left -= sz;

  //  如果全部释放，那么
  if(vma->left == 0)
  {
    // printf("release a vma\n");
    vma->valid = 0;
    vma->left = 0;
    fileclose(vma->f);
  }

  // printf("=============sys_munmap end===============\n");
  return 0;
}