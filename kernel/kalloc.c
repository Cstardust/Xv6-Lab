// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.


#define IDX(pa) ((pa - KERNBASE) / PGSIZE)
//  每一个physical page的引用计数
#define MAXN ((PHYSTOP - KERNBASE) / PGSIZE + 5)
//  仅限DRAM进行引用计数 即 pa位于 KERNBASE PHYSTOP之间
struct ref{
  struct spinlock lock;
  int ref_cnt;
} refs[MAXN];

struct run {
  struct run *next;
  // int ref_cnt;   不能放在run里面（因为run也是page的一部分，一旦被从freelist上拿下给别人，那么其reg_cnt就被覆盖5了）
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;


void
kinit()
{
  for(int i=0;i<MAXN;++i)
  {
    initlock(&refs[i].lock,"ref_lock");
  }
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

int getref(uint64 pa)
{
  if(pa < KERNBASE) return 0;
  int idx = IDX(pa);
  acquire(&refs[idx].lock);
  int cnt = refs[idx].ref_cnt;
  release(&refs[idx].lock);
  return cnt;
}

//  仅限DRAM处的进行引用计数
void incref(uint64 pa)
{
  if(pa < KERNBASE) return ;
  int idx = IDX(pa);
  // printf("pa = %p , KERNBASE = %p , idx = %d\n",pa,KERNBASE,idx);
  acquire(&refs[idx].lock);
  ++refs[idx].ref_cnt;
  release(&refs[idx].lock);
}

//  仅限DRAM处的进行引用计数
static void clearref(uint64 pa)
{
  if(pa < KERNBASE) return ;
  int idx = IDX(pa);
  acquire(&(refs[idx].lock));
  refs[idx].ref_cnt = 0;
  release(&(refs[idx].lock));
}

//  仅限DRAM处的进行引用计数
void decref(uint64 pa)
{
  if(pa < KERNBASE) return ;
  int idx = IDX(pa);
  acquire(&(refs[idx].lock));
  int c = --refs[idx].ref_cnt;
  if(c == -1) panic("ref_cnt is expected to be > 0");
  release(&(refs[idx].lock));
}


static void kinitfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);  

  clearref((uint64)pa);
}


void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kinitfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  //  check whether the pa is legal
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);


  int idx = ((uint64)pa - KERNBASE) / PGSIZE;
  if(idx <= 0 ) panic("idx is expected to be > 0 ");

  acquire(&(refs[idx].lock));

  if(--refs[idx].ref_cnt < 0) panic(">= 0");

  if((refs[idx].ref_cnt) == 0)  
  {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
    r = (struct run*)pa;
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  release(&(refs[idx].lock));
  
  release(&kmem.lock);
}

// extern pagetable_t kernel_pagetable;
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  // if((uint64)r >= KERNBASE && (uint64)r<=PHYSTOP)
  if((char*)r >= end && (uint64)r < PHYSTOP)
  {
    if(getref((uint64)r)!=0)
    {
      printf("pa = %p , cnt = %d\n",r,getref((uint64)r));
      // vmprint(kernel_pagetable,0);
      panic("expected to be 0\n");
    }
  }

  // ++ref
  //  0.0版本：incref((uint64)r);
  //  1.0版本：错误的认为不应当在这里，应当在建设映射 如 mappages 的时候++ref 
  //  2.0版本
  if(r)
  {
    memset((char*)r, 5, PGSIZE); // fill with junk
    incref((uint64)r);
  }
  return (void*)r;
}
