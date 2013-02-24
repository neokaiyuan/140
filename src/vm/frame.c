#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
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
  frame_table = (struct frame_entry *) malloc (sizeof(struct frame_entry) 
                                               * user_pages);
  lock_init (&frame_table_lock);
}

static struct frame_entry *
kpage_to_frame_entry (void *kpage)
{
  void *phys_addr = (void *) (kpage - PHYS_BASE);
  int index = (unsigned) phys_addr / PGSIZE;
  return &frame_table[index];
}

/* allocates page in physical memory for a specific thread's virtual memory */
void *
frame_add (struct thread *thread, void *upage, bool zero_page, bool pinned) 
{
  lock_acquire (&frame_table_lock);

  void *kpage = zero_page ? palloc_get_page (PAL_USER | PAL_ZERO) 
                          : palloc_get_page (PAL_USER); 
  if (kpage == NULL) {
    //Will implemenent swping to swp disk here
    ASSERT(true);
  }
  struct frame_entry *entry = kpage_to_frame_entry (kpage);
  entry->thread = thread;
  entry->upage = upage;
  entry->pinned = pinned;

  lock_release (&frame_table_lock);
  return kpage;
}

void
frame_remove (void *kpage)
{
  //lock_acquire (&frame_table_lock);

  struct frame_entry *entry = kpage_to_frame_entry (kpage);
  //palloc_free_page (kpage);
  entry->upage = entry->thread = NULL;

  //lock_release (&frame_table_lock);
}

/*
    This function will pin or unpin a page of memory
    given its upage, note it assumes that the caller
    has already acquired the frame table lock.
 */
static void
change_pin_status (bool pinned, void *upage) 
{
  struct thread *t = thread_current ();
  void *kpage = pagedir_get_page (t->pagedir, upage);
  ASSERT (kpage!= NULL);
  struct frame_entry *entry = kpage_to_frame_entry (kpage);
  entry->pinned = pinned;
}

/* 
    This function will pin memory at upage extending size bytes. This 
    memory cannot be changed again until it is unpinned. this
    restriction can be circumvented by a process when it exits.
 */
void
frame_pin_memory (void *upage, int length)
{
  lock_acquire (&frame_table_lock);

  int first_page_offset = (unsigned) upage % PGSIZE;
  change_pin_status (true, upage);
  length -= (PGSIZE - first_page_offset);
  while (length > 0) {
    upage += PGSIZE; 
    change_pin_status (true, upage);
    length -= PGSIZE;
  }

  lock_release (&frame_table_lock);
}

/*
    The sister function to frame_pin_memory. Unpins a region
    of physical memory corrosponded to by upage and extending
    size bytes.
*/
void frame_unpin_memory (void *upage, int length) 
{
  lock_acquire (&frame_table_lock);

  int first_page_offset = (unsigned) upage % PGSIZE;
  change_pin_status (false, upage);
  length -= (PGSIZE - first_page_offset);
  while (length > 0) {
    upage += PGSIZE; 
    change_pin_status (false, upage);
    length -= PGSIZE;
  }

  lock_release (&frame_table_lock);
}
