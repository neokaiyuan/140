#include <stdio.h>
#include "devices/block.h"
#include "threads/thread.h"
#include "vm/frame.h"
#include "vm/page.h"

#define SECTORS_PER_PAGE 8

static unsigned 
page_hash_hash_func (const struct hash_elem *e, void *aux)
{
  struct sup_page_entry *entry = hash_entry (e, struct sup_page_entry, elem);
  return sup_page_entry->upage;
}

static bool
page_hash_less_func (const struct hash_elem *a, const struct hash_elem *b,
                     void *aux)
{
  unsigned key_a = page_hash_hash_func (a, NULL);
  unsigned key_b = page_hash_hash_func (b, NULL);
  return key_a < key_b;
}

struct hash *
page_init()
{
  struct hash *sup_page_table = malloc (sizeof(struct hash));
  assert (sup_page_table != NULL);
  hash_init (sup_page_table, &page_hash_hash_func, &page_hash_less_func, NULL);
  return sup_page_table;
}

/* map a page to user virtual memory, but not physical memory */
void
page_add_unmapped (struct hash *sup_page_table, void *upage, void *kpage,
          enum page_type, enum page_loc, int swap_index, struct file *file,
          int file_offset, bool zeroed, bool writeable)
{
  struct sup_page_entry *entry = malloc (sizeof(struct sup_page_entry));
  entry->upage = upage;
  entry->kpage = kpage;
  entry->page_type = page_type;
  entry->page_loc = page_loc;
  entry->swap_page_index = swap_index;
  entry->file = file;
  entry->file_offset = file_offset;
  entry->zeroed = zeroed;
  entry->writable = writable;

  hash_insert (sup_page_table, &entry->elem);
}

static void
read_page_from_disk (struct block *block, int page_index, void *buffer)
{
  int i;
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_read (block, page_index * SECTORS_PER_BLOCK + i, 
                buffer + i * BLOCK_SECTOR_SIZE);
  }  
}

static void
write_page_to_disk (struct block *block, int page_index, void *buffer)
{
  int i;
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_write (block, page_index * SECTORS_PER_BLOCK + i, 
                buffer + i * BLOCK_SECTOR_SIZE);
  }  
}

/* unmap a page from physical memory and user virtual memory */
void
page_remove (struct hash *sup_page_table, void *upage) 
{
  struct thread *t = thread_current ();
  lock_acquire (&t->sup_page_table_lock);

  struct sup_page_entry dummy;
  dummy.upage = upage;
  struct sup_page_entry *entry = hash_find (sup_page_table, &dummy.elem);
 
  if (entry->page_loc == MAIN_MEMORY) {

    if (entry->page_type == _FILE && entry->writeable && 
        pagedir_is_dirty (t->pagedir, upage)) {
      lock_acquire (&filesys_lock);
      file_write_at (entry->file, upage, PGSIZE, entry->file_offset);
      lock_release (&filesys_lock);
    }
    frame_remove (entry->kpage); 

  } else if (entry->page_lock == SWAP_DISK) {

    if (entry->page_type == _FILE && entry->writeable &&
        pagedir_is_dirty (t->pagedir, upage)) {
      lock_acquire (&filesys_lock);

      char buffer[PGSIZE];
      read_page_from_disk (block_get_role (BLOCK_SWAP), entry->swap_page_index,
                           buffer);
      file_write_at (entry->file, buffer, PGSIZE, entry->file_offset);      

      lock_release (&filesys_lock);
    }
    //Remove from swap disk
  }

  hash_delete (sup_page_table, &entry->elem);
  free (entry);

  lock_release (&t->sup_page_table_lock);
  
  // THINK ABOUT WHAT HAPPENS FOR MMAPPED FILE
}

/* map an address into main memory */
void
page_map (void *upage)
{
  ASSERT (upage % PGSIZE == 0);

  struct thread *t = thread_current ();
  lock_acquire(&thread->sup_page_table_lock);

  struct sup_page_entry dummy;
  dummy.upage = upage;
  struct sup_page_entry *entry = hash_find (sup_page_table, &dummy.elem); 

  if (entry->page_loc == UNMAPPED) {
    void *k_page = frame_add (thread_current(), upage, entry->zeroed);
    if (k_page == NULL) {
      // evict something
    }
    
    entry->kpage = kpage;
    entry->page_loc = MAIN_MEMORY;
    pagedir_set_page (t->pagedir, upage, kpage, entry->writeable); //check this

    if (entry->page_type == _FILE || entry-page_type == _EXEC){
      lock_acquire (&filesys_lock);
      file_read_at (entry->file, upage, PGSIZE, entry->file_offset);
      lock_release (&filesys_lock);
    }
  
  } else if (entry->page_loc == SWAP_DISK) {
    // Related to evict
  }
  lock_release (&t->sup_page_table_lock);
}
