#include "threads/thread.h"

struct frame_entry {
  struct thread *thread;
  void *user_vaddr;
}
 

void frame_init ();
void *frame_add (struct thread *thread, void *user_vaddr);
void frame_remove (struct frame_entry *entry);


