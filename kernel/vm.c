#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
//  建立好kernel_pagetable 但并没有启动pgtbl
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}


pagetable_t kvminitproc()
{
  pagetable_t kpg = (pagetable_t) kalloc();
  memset(kpg,0,PGSIZE);

  // uart registers
  kvmmapproc(kpg,UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmapproc(kpg,VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmapproc(kpg,PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmapproc(kpg,KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmapproc(kpg,(uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmapproc(kpg,TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
  
  return kpg;
}


// add a mapping to the kernel page table.
// does not flush TLB or enable paging.
//  在proc_kernel_pagetable上，建立从va到pa映射，大小为sz。建立失败则panic。仅仅建立映射，而不负责kalloc映射到的physical page
void kvmmapproc(pagetable_t proc_kernel_pagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(proc_kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}



// Switch h/w page table register to the kernel's page table,
// and enable paging.
//  设置cpu当前使用页表为全局的kernel_pagetable
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}


void kvminithartproc(pagetable_t kpgtbl)
{
  w_satp(MAKE_SATP(kpgtbl));
  sfence_vma();
}


// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
//  遍历前两级页表，找到第三级页表，返回va对应的第三级页表的叶子PTE , 这个PTE有可能全0
//  alloc == 1 
  //  创建遍历过程中需要遍历到的但不存在的pagetable，最后返回va对应的第三级页表的叶子PTE.如果这个pte原本是不存在的话，那么实际上返回的pte是0
  //  success : return pte的地址
  //  faile : return 0 创建失败
//  alloc == 0
  //  不必创建遍历过程中需要遍历到的但不存在的pagetable，如果需要遍历的页表不存在，那么直接返回0即可。
  //  success : return pte的地址
  //  fail : return 0 所需遍历的页表不存在
//  只有当遍历到的三级页表在遍历前就全部存在，且第三级页表中的va对应的pte也之前就建立了对物理页的映射，才会得到返回有效的指向物理页的pte。
//  貌似只有通过walk接口，才会创建三级页表。
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    //  取得level级的va对应的PTE
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      //  pte指向的下一级页表存在,那么遍历其指向的下一级页表即可
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      //  pte指向的下一级页表不存在，那么kalloc创建一个页表，并使得PTE指向该页表（这样该页表就成为下一级页表了），然后更新本PTE为有效 | PTE。
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)   //  如果alloc!=0 那么kalloc一个pagetable，并赋给pagetable
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;     //  令pte指向kalloc的页表，并更新该pte有效
    }
  }
  //  返回va对应的第三级页表的叶子PTE
  return &pagetable[PX(0, va)];
}


// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
//  根据va 去 pagetable里面查找相应的PTE , 进而得到pa。并不新增pte。也不会改变已有pte。也即pgtbl不会被改变。
  //  如果查到的PTE无效 / 无权限，返回0
  //  如果查到的PTE有效，返回pa
    //  返回的pa也只是physical page的起始addr，因为舍弃了va的低12位。
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}


// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
//  在kernel_pagetable上，建立从va到pa映射，大小为sz。建立失败则panic
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}


// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
//  在kernel pagetable中 查找va对应的pa 
//  success : return pa (PPN + offset)
//  faile : panic  一般在va对应的物理页并不存在或者没有建立映射时（即根据va查询到的pte所指向的物理页无效 PTE_V = 0）
//  仅仅对kernel stack的虚拟地址的对应的物理地址进行查询时有效。因为kernel pagetable只有kernel stack不是直接映射。（其实还有trampoline，不过那是pc指向的地址了）
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  //  在kernel_pagetable中，查找va对应的pte  
  pte = walk(kernel_pagetable, va, 0);
  //  不存在对应pte
  if(pte == 0)
    panic("kvmpa");
  //  pte指向的物理页无效 panic
  if((*pte & PTE_V) == 0) 
    panic("kvmpa");
  //  得到PPN
  pa = PTE2PA(*pte);
  //  return pa
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
//  在pagetable上（所代表的三级页表），建立从va到pa的映射。一个page一个page的建立映射，perm是指定的权限位。
//  [va,va+size-1] -> [pa,pa+size-1] 使用的虚拟地址是连续的，传入的物理内存也是连续的
//  建立映射：*pte = PA2PTE(pa) | perm | PTE_V
//  success : return 0;
//  fail : return -1;   由于walk函数在查询或者创建pagetable自身的时候失败。
//  mappages 并没有kalloc任何东西，也没有kalloc pagetable自身的物理内存，只是建立了从va到pa的映射。
//  但是mappages调用的walk 在查询的时候 可能会kalloc pagetable自身的物理内存
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  
  uint64 a, last;
  pte_t *pte;
  // printf("va = %p , sz = 0x%x , pa = %p\n",va, size, pa);
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    // 查询/创建a对应的三级页表，并返回a对应的pte
    if((pte = walk(pagetable, a, 1)) == 0)    //  查询/创建 失败
      return -1;
    if(*pte & PTE_V)                          //  如果新创建的pte原本就有效，那么失败
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;         //  建立从va到pa的映射。: va对应的PTE指向PA，并填充为valid有效；并填充其他权限位term
    if(a == last)
      break;
    a += PGSIZE;                              //  接下来该建立下一页的映射
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
// If do_free != 0, physical memory will be freed
//  解除va到pa的映射（pa由pagetable查找）（即使得va对应的pte清0），并释放pa对应的物理页(if dofree = 1)。总共解除对npages个物理页的映射
//  fail : panic。查询pte失败 | te指向的物理页无效 | ques : 查询到的pte不是个叶子?
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  //  要求传入的va必须PGSIZE对齐
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");
  //  解除va到pa的映射，(并释放pa对应的物理页)
  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    //  查询pte
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    //  pte指向的物理页无效
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    //  pte的权限位只有PTE_V这一个位1, 代表了返回的这个pte不是一个叶子pte.
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    //  dofree = 1 释放物理内存pa.即 将其放回free physical page 的 freelist上。
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    //  pte清0，也即va对应的这个pte不再映射到pa了。从此va这个虚拟地址就无效了。
    *pte = 0;
  }
}


// create an empty user page table.
// returns 0 if out of memory.
//  创建一个PGSIZE大小的empty的user的pagetable(通过kalloc) 
//  success ：return pagetable
//  fail    ：return 0
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}



// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
//  在(user的)pagetable上，从虚拟地址的0开始，建立从 virtual [0,PGSIZE-1) 到 physical [mem,mem+PGSIZE-1) 的映射
//  src这段内存中装的是initcode的代码
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  //  请求一块dram 
  //  **返回的是kernel_pagetable维护的虚拟地址，也即物理地址**。
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  //  让pagetable维护 从 virtual [0,PGSIZE-1) 到 physical [mem,mem+PGSIZE-1) 的映射
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  //  将src的内容移动到mem中 mem，src使用的都是kernel维护的虚拟地址（即物理地址）
  memmove(mem, src, sz);
}



// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
//  传入的oldsz 和 newsz 都是虚拟地址
//  为user pagetable 建立从virtual addr oldsz到virtual addr newsz的虚拟地址空间。为其申请物理内存并建立映射。
//  success : return newsz 返回pagetable维护的最大的虚拟地址
//  fail : return 0;   当kalloc申请物理内存失败 / 建立mappages映射失败
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();   //  申请物理内存 返回的mem是kernelpagetable维护的虚拟地址 即物理地址
    if(mem == 0){     //  失败 释放pagetable维护的从oldsz到a的内存（解除映射，并释放physical memory)
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);   
    //  已经申请好physical mem , 接下来只需要在pagetable上 建立从va到mem pa的映射
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      //  失败 则将mem归还给freelist
      kfree(mem);
      //  将从[oldsz,a]的建立的映射销毁以及释放映射到的物理内存
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
//  作用：pagetable维护的虚拟地址空间 从oldsz减到newsz 
  //  解除va到pa的映射 并释放物理内存
//  传入的oldsz和newsz都是pagetable上的虚拟地址
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    //  将newsz到oldsz的物理页释放 并解除这段虚拟地址的维护。
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }
  return newsz;
}


//  释放以pgtbl为根节点的所有pgtbl，并将pgtbl的所有pte清0，但不释放映射到的物理内存
void freepgtblonly(pagetable_t pgtbl)
{
  for(int i=0;i<512;++i)
  {
    pte_t pte = pgtbl[i];
    //  前两级的pte
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0)
    {
      uint64 child = PTE2PA(pte);
      freepgtblonly((pagetable_t)child);   //  释放下一级pte
      // pgtbl[i] = 0;                   //   PTE清0
    }
    // //  叶子pte清0（感觉也就没必要清0 因为待会直接释放pte自身）
    // else
    // {
    //   pgtbl[i] = 0;
    // }
  }
  //  释放整个pgtbl自身
  kfree((void*)pgtbl);
}


// Recursively free page-table pages.
// All leaf mappings must already have been removed.
//  释放所有存在的pagetable，并且检测叶子page table上面的叶子 PTE 是否已经没有对dram的映射,如果害有对dram的映射，那么panic
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    //  有效的level2 level1的pgtbl的pte
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);   //  dfs 释放
      pagetable[i] = 0;               //  将PTE清0
    } // else 叶子pte
    //  有效的叶子pte 
    else if(pte & PTE_V){
      panic("freewalk: leaf");        //  如果叶子页表的虚拟地址还有映射到物理地址，报错panic。因为调用freewalk之前应当先uvmunmap释放物理内存
    }
    //  无效的叶子pte
  }
  //  释放整个pagetable自身占据的物理内存
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
//  sz > 0 时 释放user page table 所建立的 虚拟地址 到 物理地址的映射，并释放其所映射的物理内存
//  释放user page table 自身
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}


//  当user的pgtbl 有任何改动(增添或删除)的时候 将改动的pte拷贝给kernel pgtbl
//  将user pgtbl 的pte拷贝给user kernel pagetable [start,end)范围最大为[0,0x0C000000-1];
int u2kvmcopymappingonly(pagetable_t kpgtbl,pagetable_t pgtbl,uint64 start,uint64 end)
{
  if(start > end) return -1;
  if(end >= PLIC) return -1;

  for(uint64 i=PGROUNDUP(start);i<end;i+=PGSIZE)   //  i : va
  {
    //  user leaf pte
    pte_t *pte = walk(pgtbl,i,0);
    //  pte 不存在
    if(pte == 0) 
      panic("u2kvmvopy : user pgtbl pte should exist");
    if((*pte & PTE_V) == 0){
      printf("va = %p , pte = %p , start = %p , end = %p\n",i,*pte,start,end);
      panic("u2kvmvopy : page pte pointed should be valid");
    }
    uint64 pa = PTE2PA(*pte);
    uint64 flags = PTE_FLAGS(*pte) & ~PTE_U;
    if(mappages(kpgtbl,i,PGSIZE,pa,flags)!=0)
    {
      // panic("u2kvmvopy : mappages failed");
      uvmunmap(kpgtbl,0,i/PGSIZE,0);
      return -1;
    }
  }

  return 0;
}

//  解除proc 的 kernel pgtbl 的va从 [newsz,oldsz-1] 到pa的映射（即将相应pte清0），但不释放physical memory
//  [newsz , oldsz-1] 应当包含于 [0,PLIC-1]
uint64 kvmdeallocpgtblonly(pagetable_t kpgtbl, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;
  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(kpgtbl , PGROUNDUP(newsz),npages,0);
  }
  return newsz;
}


// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.

//  传入的old pgtbl 所维护的映射是从[0,sz-1]到physical memory的映射
//  所谓uvmcopy 就是将old pgtbl所建立的映射关系以及物理内存 拷贝给 new pgtbl
//  将old(parent) page table 所维护的虚拟地址到 物理地址的映射 中 物理地址的 **内容** **拷贝**给 
//  **new pagetable** 所建立的虚拟地址所对应的物理内存.
//  本函数令 new pgtbl 也像old pgtbl 一样，建立从[0,sz-1]到physical memory的映射.
//  success : return 0
//  fail : return -1
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    //  从parent pgtbl 上寻找va为i 对应的 pte
      //  pte不存在
    if((pte = walk(old, i, 0)) == 0)    
      panic("uvmcopy: pte should exist");
      //  pte指向的物理页不存在  
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    //  得到old pgtbl 维护的 va(i) 对应物理内存pa
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    //  为new userpagetable 分配物理内存失败
    if((mem = kalloc()) == 0)
      goto err;
    //  将pa对应的physical oage的内容拷贝到新为new userpgtbl准备的物理内存mem page中
    memmove(mem, (char*)pa, PGSIZE);
    //  建立映射。
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
 //  error : 取消为new userpgtbl 建立的映射 以及释放 刚刚分配的物理内存 
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
//  将va在user pagetable对应的PTE标记为user无权限
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
  //  proc kernel pgtbl 不必同步这里 因为其同步的时候就已经设置成~PTE_U的了
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
//  从kernel pagetable维护的src 的内容拷贝 len bytes 到 user pagetable维护的dst
//  success : return 0
//  fail : return -1
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
//  从user pagetable维护的srcva 的内容拷贝 len bytes 到 kernel pagetable维护的dst
//  success : return 0
//  fail : return -1 . 查询srcva对应pa失败
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  // uint64 n, va0, pa0;

  // while(len > 0){
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);   //  翻译成了物理地址
  //   if(pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if(n > len)
  //     n = len;
  //   //  这里可以成功运行 也不过是因为kernel_pagetable 在这个部分对 dram是恒等映射？
  //   //  因为cpu还是会将pa当作kernel pagetable的va来进行翻译。翻译成pa。
  //   memmove(dst, (void *)(pa0 + (srcva - va0)), n);

  //   len -= n;
  //   dst += n;
  //   srcva = va0 + PGSIZE;
  // }
  // return 0;
  return copyin_new(pagetable,dst,srcva,len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
//  将user pagetable维护的srcva的地址的内容，拷贝kernel pagetable维护的dst。直到遇到'\0'
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable,dst,srcva,max);
  // uint64 n, va0, pa0;
  // int got_null = 0;

  // while(got_null == 0 && max > 0){
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if(pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if(n > max)
  //     n = max;

  //   char *p = (char *) (pa0 + (srcva - va0));
  //   while(n > 0){
  //     if(*p == '\0'){
  //       *dst = '\0';
  //       got_null = 1;
  //       break;
  //     } else {
  //       *dst = *p;
  //     }
  //     --n;
  //     --max;
  //     p++;
  //     dst++;
  //   }

  //   srcva = va0 + PGSIZE;
  // }
  // if(got_null){
  //   return 0;
  // } else {
  //   return -1;
  // }
}

// check if use global kpgtbl or not
//  检验当前使用的pagetable是全局的kernel pagetable 还是其他pagetable
//  通过$satp比较 
int 
test_pagetable()
{
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(kernel_pagetable);
  return satp != gsatp;
}




static void Assert(int flag,char *msg)
{
  if(!flag)
  {
    panic(msg);
  }
}

void vmprint(pagetable_t pagetable,int depth)
{
  if(depth > 2) return ;
  Assert(depth>=0 && depth<=2,"vm print depth error");
  if(depth == 0)
    printf("page table %p\n",(void*)pagetable);

  //  pagetable = 512 ptes = 512 uint64 = 512 * 8 bytes = 4096 bytes
  for(int i=0;i<512;++i)
  {
    pte_t pte = pagetable[i];
    if(pte & PTE_V)
    {
      uint64 next = PTE2PA(pte);    //  nextlevel_pagetable_or_mem_pa
      int cp = depth;
      while((cp--)>=0) {printf("||");if(cp!=-1) printf(" ");}
      printf("%d: pte %p pa %p\n",i,(void*)pte,next);
      vmprint((pagetable_t)next,depth+1);
    }
  }
  return ;
}
