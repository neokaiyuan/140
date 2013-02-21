#include "threads/thread.h"

struct frame_entry {
  struct thread *thread;
  void *upage;
};
 
void frame_init (size_t user_page_limit);
void *frame_add (struct thread *thread, void *upage, bool zero_page);
void frame_remove (void *kpage);
