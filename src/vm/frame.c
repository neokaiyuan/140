#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "vm/frame.h"

static struct frame_entry *frame_table;
static struct lock frame_table_lock;

void
frame_init (size_t user_page_limit)
{
  uint8_t *free_start = ptov (1024 * 1024);
  uint8_t *free_end = ptov (init_ram_pages * PGSIZE);
  size_t free_pages = (free_end - free_start) / PGSIZE;
  size_t user_pages = free_pages / 2;
  if (user_pages > user_page_limit)
    user_pages = user_page_limit;
  frame_table = malloc (sizeof(frame_entry) * user__pages);
  lock_init (&frame_table_lock);
}

void *
frame_add (struct thread *thread, void *user_vaddr) 
{
  lock_acquire (&frame_table_lock);

  void *kernel_vaddr = palloc_get_page(0); 
  if (kernel_vaddr == NULL) {
    //Will implemenent swping to swp disk here
    ASSERT(true);
  }
  void *phys_addr = kernel_vaddr - PHYS_BASE;
  int index = phys_addr / PGSIZE;
  struct frame_entry *entry = &frame_table[index];
  frame_entry->thread = thread;
  frame_entry->user_vaddr = user_vaddr;

  lock_release (&frame_table_lock);
  return phys_addr;
}

void
frame_remove (struct frame_entry *entry)
{
  lock_acquire (&frame_table_lock);

  free (entry->user_vaddr);
  entry->user_vaddr = entry->thread = NULL;

  lock_release (&frame_table_lock);
}
