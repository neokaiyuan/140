#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"

struct frame_entry {
  struct thread *thread;  // to access the thread's pagedir
  void *upage;            // to access the entry in thread's pagedir
  bool pinned;         //If true, pinned by the kernel, do not evict
};
 
void frame_init (size_t user_page_limit);
void *frame_add (struct thread *thread, void *upage, bool zero_page, bool pinned);
void frame_remove (void *kpage);
void frame_pin_memory (void *upage, int length);
void frame_unpin_memory (void *upage, int length);

#endif
