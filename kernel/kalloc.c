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

//  这个end是从哪来的？？？哪里定义的？？？
extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
//  这分配的page是从哪里来的？disk ? dram ? 我的程序不是本身就运行在dram?

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];


char *kmem_lock_name[NCPU] = {
  "kmem_0",
  "kmem_1",
  "kmem_2",
  "kmem_3",
  "kmem_4",
  "kmem_5",
  "kmem_6",
  "kmem_7",
};


void showPhysicalPage()
{
  printf("===============showPhysicalPage==============\n");
  push_off();
  int cpu_id = cpuid();
  pop_off();
  
  struct run *cur = kmem[cpu_id].freelist;
  do
  {
    printf("cpu %d , physical page %p\n",cpu_id,cur);
    if(cur!=0)
      cur = cur->next;
  } while (cur != 0);
  printf("===============endShowPhysicalPage==============\n");
}

void
kinit()
{
  for(int i=0;i<NCPU;++i)
  {
    initlock(&kmem[i].lock, kmem_lock_name[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)     // kernel使用的虚拟地址 直接映射到物理地址
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  printf("p kenrel dram base virtual address = %p\n",p);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  if(((uint64)pa % PGSIZE)!=0 || ((char*)pa <end) || (uint64)pa>=PHYSTOP)
  {
    panic("kfree");
  }

  memset((char*)pa,1,PGSIZE);
  struct run *r = pa;


  push_off();
  int cpu_id = cpuid();
  pop_off();

  acquire(&kmem[cpu_id].lock);

  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;

  release(&kmem[cpu_id].lock);
}

static void Assert(int flag)
{
  if(flag == 0){
    printf("Assert\n");
    exit(-1);
  }
}

// cpu_id 向other cpus 偷取 至多num个physical page。
void steal(int cpu_id,int page_num)
{
  // printf("===============start stealing===============\n");
  
  push_off();
  int check = cpuid();
  pop_off();
  Assert(check == cpu_id);

  struct run * tail = kmem[cpu_id].freelist;
  for(int i=0;i<NCPU;++i)
  {
    if(i == cpu_id) continue; 

    acquire(&kmem[i].lock);

    if(kmem[i].freelist == 0) {
      release(&kmem[i].lock);
      continue;   //  无该cpu核 / 该cpu也没 physical page
    }

    struct run *page_stolen = kmem[i].freelist;
    struct run *pre = 0;
    while(kmem[i].freelist && page_num > 0)
    {
      pre = kmem[i].freelist;
      kmem[i].freelist = kmem[i].freelist->next;
      --page_num;
    }
    if(pre)
      pre->next = 0;
    
    if(kmem[cpu_id].freelist == 0)
    {
      kmem[cpu_id].freelist = page_stolen;
      tail = pre;
    }
    else
    {
      tail->next = page_stolen;
      if(pre!=0)
        tail = pre; 
    }

    release(&kmem[i].lock);     

    if(page_num == 0)           //  BUG!!!! 之前先break后release 造成没release
    {
      break;
    }
  }

  // if(kmem[cpu_id].freelist == 0)
  // {
  //   printf("cpu %d steal physical page %p pagenum left %d\n",cpu_id,kmem[cpu_id].freelist,page_num);
  // }
  // {
  //   struct run *r = kmem[cpu_id].freelist;
  //   do{
  //     printf("cpu %d steal physical page %p\n",cpu_id,r);
  //     if(r)
  //       r = r->next;
  //   }while(r!=0);
  // }
  // printf("===============end stealing===============\n");
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
//  free page list : pa ---> pb ---> pc ---> pd  , 将pa分配给user
void *
kalloc(void)
{
  push_off();
  int cpu_id = cpuid();
  pop_off();

  //  除了cpu0，其余cpu核一开始没分配 physical page
  struct run *res = kmem[cpu_id].freelist;

  // int branch_flag = -1;

  if(!res)
  {
    // branch_flag = 0;
    acquire(&kmem[cpu_id].lock);
    steal(cpu_id,256);               __sync_synchronize();
    res = kmem[cpu_id].freelist;    __sync_synchronize();
    if(res)
      kmem[cpu_id].freelist = kmem[cpu_id].freelist->next;
    else{
      // printf("cpu %d freelist %p\n",cpu_id,kmem[cpu_id].freelist);
    }
    release(&kmem[cpu_id].lock);
  }
  else
  {
    // branch_flag = 1;

    acquire(&kmem[cpu_id].lock);
    kmem[cpu_id].freelist = res->next;    // 取下page
    release(&kmem[cpu_id].lock);
              
  } 

  if(res == 0){
    // printf("cpu %d branch %d alloc for page %p \n",cpu_id,branch_flag,res);
  }else{
    memset((char*)res,5,PGSIZE);
  }

// if(res){
  // memset((char*)res,5,PGSIZE);            //  fill with junk
// }
  
  // printf("cpu %d alloc for page %p\n",cpu_id,res);
  return (void*)res;
}




