/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "spike_interface/spike_utils.h"
#include "memlayout.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char *buf, size_t n)
{
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert(current);
  char *pa = (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)buf);
  sprint(pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code)
{
  sprint("User exit with code:%d.\n", code);
  // in lab1, PKE considers only one app (one process).
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page()
{
  void *pa = alloc_page();
  uint64 va = g_ufree_page;
  g_ufree_page += PGSIZE;
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));

  return va;
}

uint64 sys_user_better_malloc(int n)
{
  int i, j, t;
  uint64 va = 0;
  void *pa;
  char *bitmap = current->bitmap;
  n = n/256+1;
  t = 1;
  for(i = 0;i<128;i++)
  {
    if (bitmap[i])
      continue;
    if(t)
    {
      va = USER_FREE_ADDRESS_START + i * 256;
      t = 0;
    }
    if (!(i % 16))
    {
      for(j = 0;j<16;j++)
      {
        // sprint("%d\n", bitmap[j]);
        if(bitmap[j]) break;
      }
      if(j==16){
        pa = alloc_page();
        // sprint("%x\n", USER_FREE_ADDRESS_START + i * 256);
        // sprint("0\n");
        user_vm_map((pagetable_t)current->pagetable, USER_FREE_ADDRESS_START + i * 256, PGSIZE, (uint64)pa,
                    prot_to_type(PROT_WRITE | PROT_READ, 1));
      }
    }
    bitmap[i] = 1;
    n--;
    if(n==0) break;
  }
  // for (i = 0; i < 8; i++)
  // {
  //   t = 0;
  //   for (j = 0; j < 16; j++)
  //     if (bitmap[j])
  //       t++;
  //   if (t == 0)
  //   {
  //     for (j = 0; j < n / 256 + 1; j++)
  //     {
  //       bitmap[i * 8 + j] = 1;
  //       // for (j = 0; j < 16; j++)
  //       //   sprint("%d", bitmap[i * 8 + j]);
  //     }

  //     pa = alloc_page();
  //     va = USER_FREE_ADDRESS_START + i * PGSIZE;

  //     // sprint("0\n");
  //     user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
  //                 prot_to_type(PROT_WRITE | PROT_READ, 1));
  //     break;
  //   }
  //   else if (t == 16)
  //     continue;
  //   else
  //   {
  //     t = -1;
  //     for (j = 0; j < 16; j++)
  //     {
  //       if (bitmap[i * 8 + j] == 0)
  //       {
  //         if (t == -1)
  //           t = j;
  //         if (j - t + 1 > n / 256)
  //           break;
  //       }
  //       else
  //         t = -1;
  //     }
  //     if (j == 16)
  //       continue;
  //     else
  //     {
  //       for (j = t; j < t + n / 256 + 1; j++)
  //       {
  //         bitmap[i * 8 + j] = 1;
  //       }
  //       // for (j = 0; j < 16; j++)
  //       //   sprint("%d", bitmap[i * 8 + j]);
  //       // sprint("%d\n", t);
  //       va = USER_FREE_ADDRESS_START + t * 256;
  //       // sprint("%x\n", va);
  //       break;
  //     }
  //   }
  // }
  // sprint("%x\n", va);
  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va)
{

  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);

  return 0;
}

uint64 sys_user_better_free(uint64 va)
{
  // sprint("%llx %llx\n",va,USER_FREE_ADDRESS_START);
  int x = va, y = USER_FREE_ADDRESS_START;

  int i = (x-y) / PGSIZE;
  int j = (x-y - i * PGSIZE) / 256;
  // sprint("%d %d",i,j);
  char *bitmap = current->bitmap;

  bitmap[0 * 8 + j] = 0;
  int t = 0;

  for (j = 0; j < 16; j++)
    if (bitmap[i * 8 + j])
      t++;
  if (t == 0)
  {
    user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
    // sprint("1\n");
  }
  return 0;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7)
{
  switch (a0)
  {
  case SYS_user_print:
    return sys_user_print((const char *)a1, a2);
  case SYS_user_exit:
    return sys_user_exit(a1);
  // added @lab2_2
  case SYS_user_allocate_page:
    return sys_user_better_malloc(a1);
  case SYS_user_free_page:
    return sys_user_better_free(a1);
  default:
    panic("Unknown syscall %ld \n", a0);
  }
}
