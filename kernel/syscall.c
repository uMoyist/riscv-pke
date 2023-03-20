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
  if (n == 0)
    return 0;
  uint64 *bitmap;
  ;
  const uint16 *page_used = (uint16 *)current->bitmap;
  int size = (n + 8) / 256 + 1; //one byte for size;
  int i, j;
  int start = -1;
  void *pa = 0;
  uint64 va = 0;

  for (i = 0; i < 128; i++)
  {
    if (i < 64)
    {
      bitmap = current->bitmap;
      j = i;
    }
    else
    {
      bitmap = current->bitmap + 1;
      j = i - 64;
    }

    if (!((*bitmap) & (1 << (63 - j))))
    {
      if (start == -1)
        start = i;
      if (i - start + 1 == size)
        break; // i = size + start - 1;
    }
    else
      start = -1;
  }
  if ((start == -1) || i == 128)
    return 0;
  int left = start / 16, right = i / 16;
  // take it to consideration that repeat the proccess of alloc and free
  for (j = left; j <= right; j++)
    if (page_used[j] == 0)
    {
      if (g_ufree_page == USER_FREE_ADDRESS_START + j * PGSIZE)
      {
        pa = alloc_page();
        va = g_ufree_page;
        g_ufree_page += PGSIZE;
        user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
                    prot_to_type(PROT_WRITE | PROT_READ, 1));
      }
    }

  for (j = start; j <= i; j++)
  {
    if (j < 64)
    {
      bitmap = current->bitmap;
      *bitmap = ((*bitmap) & (1 << (63 - j)));
    }
    else
    {
      bitmap = current->bitmap + 1;
      *bitmap = ((*bitmap) & (1 << (127 - j)));
    }
  }
  va = USER_FREE_ADDRESS_START + start * 256;
  // sprint("%x, %x, %d, %d, %d\n", va, g_ufree_page, size, start, i);
  *((uint64 *)(lookup_pa(current->pagetable, va))) = size;
  // sprint("here\n");
  return va + 8;
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
  va = va - 8;
  int x = va, y = USER_FREE_ADDRESS_START;

  int start = (x - y) / 256;
  // sprint("%d %d",i,j);
  int size = *(uint64*)(lookup_pa(current->pagetable, va));
  // sprint("%d\n",size);
  uint64 *bitmap;
  int j;
  for (j = start; j <= start + size - 1; j++)
  {
    if (j < 64)
    {
      bitmap = current->bitmap;
      *bitmap = ((*bitmap) & (~(1 << (63 - j))));
    }
    else
    {
      bitmap = current->bitmap + 1;
      *bitmap = ((*bitmap) & (~(1 << (63 - j))));
    }
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
