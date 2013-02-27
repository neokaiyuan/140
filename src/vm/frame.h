#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"
#include "vm/page.h"

struct frame_entry {
  struct lock lock;
  struct thread *thread;  // to access the thread's pagedir
  void *upage;            // to access the entry in thread's pagedir
  bool pinned;         //If true, pinned by the kernel, do not evict
};
 
void frame_init (size_t user_page_limit);
void *frame_add (struct sup_page_entry *page_entry, bool swap, bool pinned);
void frame_remove (void *kpage);
bool frame_pin (const void *upage);
bool frame_unpin (void *upage);

#endif
