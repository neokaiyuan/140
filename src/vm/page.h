#include "lib/kernel/hash.h"

enum page_type {
  _STACK = 0;
  _EXEC = 1;
  _FILE = 2;
}

enum page_loc {
  MAIN_MEMORY = 0;
  SWAP_DISK = 1;
  UNMAPPED = 2;
}

struct sup_page_entry {
  void *upage;            //The user virtual address
  struct hash_elem elem;

  void *kpage;          // to access relevant entry in frame_table 
  enum page_type;
  enum page_loc;

  int swap_index;

  struct file *file;
  int file_offset;

  bool zeroed;
  bool writeable;
}

struct hash *page_init();
void page_add (struct hash *sup_page_table, void *upage, void *kpage,
               enum page_type, enum page_loc, int swap_index, 
               struct file *file, int file_offset, bool zeroed,
               bool writeable);
void page_remove (struct hash *sup_page_table, hash_elem *elem);

//void page_evict();

