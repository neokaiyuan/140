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
#include "vm/swap.h"

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
  return (unsigned ) key_a < (unsigned) key_b;
}

struct hash *
page_init ()
{
  struct hash *sup_page_table = malloc (sizeof(struct hash));
  if (sup_page_table == NULL)
    return NULL;
  hash_init (sup_page_table, &page_hash_hash_func, &page_hash_less_func, NULL);
  return sup_page_table;
}

/* map a page to user virtual memory, but not physical memory */
void
page_add_entry (struct hash *sup_page_table, const void *upage, void *kpage,
                int page_type, int page_loc, int swap_index, 
                int page_read_bytes, struct file *file, int file_offset, 
                bool zeroed, bool writable)
{
  struct thread *t = thread_current ();
  lock_acquire (&t->sup_page_table_lock);
  
  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = malloc (sizeof(struct sup_page_entry));
  ASSERT (entry != NULL);

  lock_init (&entry->lock);
  entry->upage = upage;
  entry->kpage = kpage;
  entry->page_loc = page_loc;
  entry->page_type = page_type;
  entry->swap_index = swap_index;
  entry->page_read_bytes = page_read_bytes;
  entry->file = file;
  entry->file_offset = file_offset;
  entry->zeroed = zeroed;
  entry->writable = writable;
  entry->written = false; // used for executables swapping in and out multiple times

  hash_insert (sup_page_table, &entry->elem);

  lock_release (&t->sup_page_table_lock); //safe because of large lock
}

//Must be locked with sup page table lock
static struct sup_page_entry *
get_sup_page_entry (struct thread *t, const void *upage)
{
  struct sup_page_entry dummy;
  dummy.upage = upage;
  struct hash_elem *e = hash_find (t->sup_page_table, &dummy.elem); 
  if (e == NULL) 
    return NULL;
  return (struct sup_page_entry *) hash_entry (e, struct sup_page_entry, elem); 
}

void 
page_remove_entry (void *upage)
{
  struct thread *t = thread_current ();
  lock_acquire (&t->sup_page_table_lock); //Since freeing cannot use small lock
  
  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  

  hash_delete (t->sup_page_table, &entry->elem);
  free (entry);

  lock_release (&t->sup_page_table_lock);
}

/* updates the sup page table and pagedir during eviction */
void
page_evict (struct thread *t, void *upage)
{
  pagedir_clear_page (t->pagedir, upage); // t cannot access during evict

  lock_acquire (&t->sup_page_table_lock);
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);   
  lock_acquire (&entry->lock);
  lock_release (&t->sup_page_table_lock);

  if (entry->page_type == _STACK) {

    entry->swap_index = swap_write_page (entry->kpage);
    entry->page_loc = SWAP_DISK;

  } else if (entry->page_type == _EXEC) {

    if (entry->writable && (pagedir_is_dirty (t->pagedir, upage) || entry->written)) {
      entry->written = true;
      entry->swap_index = swap_write_page (entry->kpage);
      entry->page_loc = SWAP_DISK;
    } else {
      entry->page_loc = UNMAPPED;
    }

  } else {
    
    if (entry->writable && (pagedir_is_dirty (t->pagedir, upage) || entry->written)) {
      entry->written = true;
      lock_acquire (&filesys_lock);
      file_write_at (entry->file, entry->kpage, PGSIZE, entry->file_offset);
      lock_release (&filesys_lock);
    }
    entry->page_loc = UNMAPPED;

  }

  entry->kpage = NULL;

  lock_release (&entry->lock);
}

/* map an address into main memory, evicting another frame if necessary */
void *
page_map (const void *upage, bool pinned)
{
  struct thread *t = thread_current ();
  lock_acquire (&t->sup_page_table_lock);

  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  
  lock_acquire (&entry->lock);
  lock_release (&t->sup_page_table_lock);

  ASSERT (entry->page_loc != MAIN_MEMORY);

  void *kpage = NULL;
  kpage = frame_add (entry, pinned); // takes care of eviction
  ASSERT (kpage != NULL)

  if (entry->page_loc == UNMAPPED) {

    if (entry->zeroed)
      memset (kpage, 0, PGSIZE);
    
    if (entry->page_type == _FILE || entry->page_type == _EXEC) {
      lock_acquire (&filesys_lock);
      file_read_at (entry->file, kpage, entry->page_read_bytes,
                    entry->file_offset);
      memset ((char *) kpage + entry->page_read_bytes, 0, 
              PGSIZE - entry->page_read_bytes);
      lock_release (&filesys_lock);
    }
  
  } else if (entry->page_loc == SWAP_DISK) {

    swap_read_page (entry->swap_index, kpage);
    entry->swap_index = -1;

  }

  entry->kpage = kpage;
  entry->page_loc = MAIN_MEMORY;

  lock_release (&entry->lock);

  return kpage;
}

/* helper function called by unmap wrappers 
   frees specified page from main memory or swap */
static void 
unmap (struct thread *t, struct sup_page_entry *entry)
{
  pagedir_clear_page (t->pagedir, entry->upage);

  if (entry->page_loc == MAIN_MEMORY) {

    if (entry->page_type == _FILE && entry->writable && 
        pagedir_is_dirty (t->pagedir, entry->upage)) {

      lock_acquire (&filesys_lock);
      file_write_at (entry->file, entry->kpage, PGSIZE, entry->file_offset);
      lock_release (&filesys_lock);
    }

    frame_remove (entry->kpage); 

  } else if (entry->page_loc == SWAP_DISK) { // file being unmapped is on swap

    if (entry->page_type == _FILE && entry->writable &&
        pagedir_is_dirty (t->pagedir, entry->upage)) {

      lock_acquire (&filesys_lock);

      char *buffer = palloc_get_page (0);
      ASSERT (buffer != NULL);
      swap_read_page (entry->swap_index, buffer);
      file_write_at (entry->file, buffer, PGSIZE, entry->file_offset);      
      palloc_free_page (buffer);

      lock_release (&filesys_lock);

    } else {
      swap_remove (entry->swap_index);
    }

    entry->swap_index = -1;
  }

  entry->kpage = NULL;
  entry->page_loc = UNMAPPED;
}

/* This function is not concurrency safe, it relies on the user to acquire
  the sup entry lock */
void 
page_unmap_via_entry (struct thread *t, struct sup_page_entry *entry)
{
  unmap (t, entry);
}

//Assumes that a lock for this is already held, ie., not concurrency safe
/* unmap a page from physical memory */
// called in syscall.c
void
page_unmap_via_upage (struct thread *t, void *upage) 
{
  lock_acquire (&t->sup_page_table_lock);

  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  
  lock_acquire (&entry->lock);
  lock_release (&t->sup_page_table_lock);

  unmap (t, entry);

  lock_release (&entry->lock);
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

bool 
page_writable (struct thread *t, const void *upage) 
{
  lock_acquire (&t->sup_page_table_lock);

  upage = pg_round_down (upage); 
  struct sup_page_entry *entry = get_sup_page_entry (t, upage);  
  lock_acquire (&entry->lock);
  lock_release (&t->sup_page_table_lock);

  bool page_writable = false;
  if (entry != NULL) 
     page_writable = entry->writable;

  lock_release (&entry->lock); 
  return page_writable;
}
