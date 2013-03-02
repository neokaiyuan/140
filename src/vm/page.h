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
  const void *upage;            //  user virtual address
  struct hash_elem elem;

  void *kpage;          // to access relevant entry in frame_table 
  int page_loc;
  int page_type;

  int swap_index;

  int page_read_bytes;  // only applies to executables and files

  struct file *file;
  int file_offset;

  bool zeroed;
  bool writable;
  bool written;
  struct lock lock;
};

/* Initializing the supp page table */
struct hash *page_init (void);

/* Adding and removing entries in the supplemental page table */
void page_add_entry (struct hash *sup_page_table, const void *upage, 
                     void *kpage, int page_type, int page_loc, int swap_index, 
                     int page_read_bytes, struct file *file, 
                     int file_offset, bool zeroed, bool writable);
void page_remove_entry (void *upage);

void page_evict (struct thread *t, const void *upage);

/*Mapping or unmapping a uaddr, this must already have a valid entry in the
  supp page table */
void *page_map (const void *upage, bool pinned);
void page_unmap_via_entry (struct thread *t, struct sup_page_entry *entry);
void page_unmap_via_upage (struct thread *t, void *upage);

/* Checking the status of pages */
bool page_entry_present (struct thread *t, const void *upage);
bool page_writable (struct thread *t, const void *upage);

//void page_evict();
#endif
