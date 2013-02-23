#ifndef VM_PAGE_H
#define VM_PAGE_H 

#include <hash.h>

#define STACK_SIZE_LIMIT 1073741824 // one gigabyte

enum page_loc {
  UNMAPPED = 0,     // unmapped to physical memory
  MAIN_MEMORY = 1,
  SWAP_DISK = 2
};

enum page_type {
  _STACK = 0,
  _EXEC = 1,
  _FILE = 2
};

struct sup_page_entry {
  void *upage;            //The user virtual address
  struct hash_elem elem;

  void *kpage;          // to access relevant entry in frame_table 
  int page_loc;
  int page_type;

  int swap_page_index;

  int page_read_bytes;  // only applies to executables

  struct file *file;
  int file_offset;

  bool zeroed;
  bool writable;
};

struct hash *page_init (void);
void page_add_entry (struct hash *sup_page_table, void *upage, void *kpage,
                     int page_type, int page_loc, int swap_index, 
                     int page_read_bytes, struct file *file, 
                     int file_offset, bool zeroed, bool writable);
void page_remove_entry (void *upage);
void *page_map (void *upage, bool pinned);
void page_unmap_via_entry (struct thread *t, struct sup_page_entry *entry);
void page_unmap_via_upage (struct thread *t, void *upage);
bool page_entry_present (struct thread *t, void *upage);

//void page_evict();
#endif
