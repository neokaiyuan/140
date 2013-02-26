#include <stdio.h>
#include <string.h>
#include "devices/block.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"

#define SECTORS_PER_PAGE 8

static unsigned 
page_hash_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct sup_page_entry *entry = hash_entry (e, struct sup_page_entry, elem);
  return (unsigned) entry->upage;
}

static bool
page_hash_less_func (const struct hash_elem *a, const struct hash_elem *b,
                     void *aux UNUSED)
{
  unsigned key_a = page_hash_hash_func (a, NULL);
  unsigned key_b = page_hash_hash_func (b, NULL);
  return key_a < key_b;
}

struct hash *
page_init ()
{
  struct hash *sup_page_table = malloc (sizeof(struct hash));
  ASSERT (sup_page_table != NULL);
  hash_init (sup_page_table, &page_hash_hash_func, &page_hash_less_func, NULL);
  return sup_page_table;
}

/* map a page to user virtual memory, but not physical memory */
void
page_add_entry (struct hash *sup_page_table, void *upage, void *kpage,
                   int page_type, int page_loc, int swap_page_index, 
                   int page_read_bytes, struct file *file, int file_offset, 
                   bool zeroed, bool writable)
{
  struct thread *t = thread_current ();
  lock_acquire (&t->sup_page_table_lock);
  
  upage = pg_round_down (upage); 

  struct sup_page_entry *entry = malloc (sizeof(struct sup_page_entry));
  entry->upage = upage;
  entry->kpage = kpage;
  entry->page_type = page_type;
  entry->page_loc = page_loc;
  entry->swap_page_index = swap_page_index;
  entry->page_read_bytes = page_read_bytes;
  entry->file = file;
  entry->file_offset = file_offset;
  entry->zeroed = zeroed;
  entry->writable = writable;

  hash_insert (sup_page_table, &entry->elem);

  lock_release (&t->sup_page_table_lock);
}

static void
read_page_from_disk (struct block *block, int page_index, void *buffer)
{
  int i;
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_read (block, page_index * SECTORS_PER_PAGE + i, 
                buffer + i * BLOCK_SECTOR_SIZE);
  }  
}

static void
write_page_to_disk (struct block *block, int page_index, void *buffer)
{
  int i;
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_write (block, page_index * SECTORS_PER_PAGE + i, 
                buffer + i * BLOCK_SECTOR_SIZE);
  }  
}

static struct sup_page_entry *
get_sup_page_entry (struct thread *t, const void *upage)
{
  struct sup_page_entry dummy;
  dummy.upage = upage;
  struct hash_elem *e = hash_find (t->sup_page_table, &dummy.elem); 
  if (e == NULL) return NULL;
  return (struct sup_page_entry *) hash_entry (e, struct sup_page_entry, elem); 
}

void 
page_remove_entry (void *upage)
{
  struct thread *t = thread_current ();
  lock_acquire (&t->sup_page_table_lock);
  
  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  

  hash_delete (t->sup_page_table, &entry->elem);
  free (entry);

  lock_release (&t->sup_page_table_lock);
}

/* map an address into main memory, evicting another frame if necessary */
void *
page_map (const void *upage, bool pinned)
{
  struct thread *t = thread_current ();
  lock_acquire (&t->sup_page_table_lock);

  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  

  void *kpage = NULL;
  /* If our entry is unmapped, map it to a physical frame */
  if (entry->page_loc == UNMAPPED) {

    kpage = frame_add (t, upage, entry->zeroed, pinned);
    if (kpage == NULL) {
      // EVICT (will return frame) & INSERT NEW PAGE
    }
    
    /* Add our new mapping of uaddr to kaddr to our pagedirectory */ 
    entry->kpage = kpage;
    entry->page_loc = MAIN_MEMORY;
    pagedir_set_page (t->pagedir, upage, kpage, entry->writable);
    
    /* If this page is a file or exxecutable, write the data in from disk */
    if (entry->page_type == _FILE || entry->page_type == _EXEC){
      lock_acquire (&filesys_lock);
      file_read_at (entry->file, kpage, entry->page_read_bytes, entry->file_offset);
      memset (kpage + entry->page_read_bytes, 0, PGSIZE - entry->page_read_bytes);
      lock_release (&filesys_lock);
    }
  
  } else if (entry->page_loc == SWAP_DISK) {

    kpage = frame_add (t, upage, entry->zeroed, pinned);
    if (kpage == NULL) {
      // EVICT (will return frame) & INSERT PAGE ON SWAP
    }
  }

  lock_release (&t->sup_page_table_lock);
  return kpage;
}

/* helper function called by unmap wrappers 
   frees specified page from main memory or swap */
static void 
unmap (struct thread *t, struct sup_page_entry *entry)
{
  pagedir_clear_page (t->pagedir, entry->upage);
  bool is_dirty = pagedir_is_dirty (t->pagedir, entry->upage);

  /* If the page being unmapped is in main memory */
  if (entry->page_loc == MAIN_MEMORY) {
    /* If the page is part of a file that is writable */
    if (entry->page_type == _FILE && entry->writable && 
        is_dirty) {
      lock_acquire (&filesys_lock);
      file_write_at (entry->file, entry->kpage, PGSIZE, entry->file_offset);
      lock_release (&filesys_lock);
    }
    /* Remove association with frame table if was in main memory */ 
    frame_remove (entry->kpage); 

  } else if (entry->page_loc == SWAP_DISK) {
    /*Case where file being unmapped is on swap */
    if (entry->page_type == _FILE && entry->writable &&
        is_dirty) {
      /* If this page is a file and on swp and writable, write if rom
         swap and back to disk  */
      lock_acquire (&filesys_lock);
      char *buffer = palloc_get_page (0);
      read_page_from_disk (block_get_role (BLOCK_SWAP), entry->swap_page_index,
                           buffer);
      file_write_at (entry->file, buffer, PGSIZE, entry->file_offset);      
      palloc_free_page (buffer);
      lock_release (&filesys_lock);
    }
    //Remove from swap disk here
  }
}


void 
page_unmap_via_entry (struct thread *t, struct sup_page_entry *entry)
{
  lock_acquire (&t->sup_page_table_lock);

  unmap (t, entry);

  lock_release (&t->sup_page_table_lock);
}

/* unmap a page from physical memory */
void
page_unmap_via_upage (struct thread *t, void *upage) 
{
  lock_acquire (&t->sup_page_table_lock);

  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  

  unmap (t, entry);

  lock_release (&t->sup_page_table_lock);
}

bool
page_entry_present (struct thread *t, const void *upage)
{
  lock_acquire (&t->sup_page_table_lock);

  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  

  lock_release (&t->sup_page_table_lock);

  if (entry == NULL) return false;
  return true;
}

bool page_writable (struct thread *t, const void *upage) 
{
  lock_acquire (&t->sup_page_table_lock);

  bool page_writable = false;
  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  
  if (entry != NULL) 
     page_writable = entry->writable;

  lock_release (&t->sup_page_table_lock);
  return page_writable;
}
