#include <hash.h>
#include <list.h>
#include <stdio.h>
#include "devices/block.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "filesys/filesys.h"

static struct list cache_list;
static struct hash cache_hash;
static struct lock cache_lock;
static struct list io_list; 
static struct lock io_lock;
static int cache_size;

struct io_entry {
  block_sector_t sector_num;
  list_elem elem;
  struct cond io_complete;
  bool evicting;          // evicting or reading in?
  struct cache_entry *ce;
};

static block_sector_t
cache_hash_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct cache_entry *entry = hash_entry (e, struct cache_entry, h_elem);
  return entry->sector_num;
}

static bool
cache_hash_less_func (const struct hash_elem *a, const struct hash_elem *b,
                      void *aux UNUSED)
{
  block_sector_t key_a = cache_hash_hash_func (a, NULL);
  block_sector_t key_b = cache_hash_hash_func (b, NULL);
  return key_a < key_b;
}

static void
flush_func (void *aux UNUSED)
{
  timer_sleep (3000);
  cache_flush (); // make sure that filesys_destroy has not been called
}

void  
cache_init ()
{
  list_init (&cache_list);
  hash_init (&cache_hash, &cache_hash_hash_func, &cache_hash_less_func, NULL);
  lock_init (&cache_lock);
  list_init (&io_list);
  lock_init (&io_lock);
  cache_size = 0;
  thread_create ("flush_thread", PRI_DEFAULT, &flush_func, NULL);
}

static struct cache_entry *
cache_find (block_sector_t sector_num)
{
  struct cache_entry dummy;
  dummy.sector_num = sector_num;
  struct hash_elem *hash_elem = hash_find (&cache_hash, &dummy.h_elem);

  if (hash_elem == NULL) {
    lock_acquire (&io_lock);

    struct io_entry ioe == NULL;
    struct list_elem *e;
    for (e = list_begin (&io_list); e != list_end (&io_list);
         e = list_next (e)) {
      ioe = list_entry (e, struct io_entry, elem);
      if (ioe->sector_num == sector_num)
        break;
    }
    
    if (ioe == NULl || ioe->sector_num != sector_num)
      return NULL;

    while (/*condition */)
      cond_wait (io_complete, &io_lock);  // wait for IO to finish

    if (ioe->evicting)
      
    // do a for loop through io_list, if in list, acquire lock
    // if evicting, release lock, return null
    // otherwise, return ce
    lock_release (&io_lock);
    return /*SOMETHING */
  } 

  struct cache_entry *ce = hash_entry (hash_elem, struct cache_entry, h_elem);
  if (ce != NULL){
    ce->pinned_cnt++;
    list_remove (&ce->l_elem);
    list_push_front (&cache_list, &ce->l_elem);
  }

  return ce;
}

static struct io_entry *
add_io_entry (block_sector_t sector_num, bool evicting, 
              struct cache_entry *ce)
{
  struct io_entry *ioe = malloc (sizeof(io_entry));
  if (ioe == NULL)
    return NULL;
  ioe->sector_num = sector_num;
  ioe->evicting = evicting;
  ioe->ce = ce;
  cond_init (&ioe->io_complete);
  list_push_back (&io_list, &ioe->elem);
  return ioe;
}

static bool
remove_io_entries (block_sector_t sector_num, struct cache_entry *ce) {
    

}

static struct cache_entry * 
add_to_cache (block_sector_t sector_num, bool zeroed)
{
  lock_acquire (&cache_lock);
  struct cache_entry *ce;
  
  ce = cache_find (sector_num); // should only return once I/O completed
  if (ce != NULL)
    return ce;

  if (cache_size >= MAX_CACHE_SIZE) {
    ASSERT (!list_empty (&cache_list));

    struct list_elem *e;
    while (true) {
      for (e = list_end (&cache_list); e != list_begin (&cache_list); 
           e = list_prev (e)) {   // iterate backwards throguh list
        ce = list_entry (elem, struct cache_entry, l_elem);
        if (ce->pinned_cnt == 0)
          break;
      }   
      if (ce->pinned_cnt == 0) {
        list_remove (&ce->l_elem);
        break;
      }
    }

    hash_delete (&cache_hash, &ce->h_elem); // nothing else finds it in cache
    
    io_entry *add_ioe = add_io_entry (sector_num, false, ce);
    if (add_ioe == NULL)
      return NULL;
    lock_acquire (add_ioe->lock);

    io_entry *evict_ioe = add_io_entry (ce->sector_num, true, ce);
    if (evict_ioe == NULL) {
      free (add_ioe);
      return NULL;
    }
    lock_acquire (evict_ioe->lock);

    lock_release (&cache_lock); // NO NEED FINE LOCK HERE B/C OUT OF HASH/LIST

    if (ce->dirty) {
      block_write (fs_device, ce->sector_num, ce->data);
      lock_acquire (&io_lock);
      cond_broadcast (&evict_ioe->io_complete, &io_lock);
      lock_release (&io_lock);
    }

  } else {
    cache_size++;
    
    ce = malloc (sizeof(struct cache_entry));
    if (ce == NULL)
      return NULL;
    //lock_init (&ce->lock);  // MAY NEED THIS

    io_entry *add_ioe = add_io_entry (sector_num, false, ce);
    if (add_ioe == NULL)
      return NULL;

    lock_release (&cache_lock);
  }
  
  if (zeroed)
    memset (ce->data, 0, BLOCK_SECTOR_SIZE);
  else
    block_read (fs_device, sector_num, ce->data);

  ce->sector_num = sector_num;
  ce->dirty = false;
  ce->pinned_cnt = 1;;
  
  lock_acquire (&cache_lock);

  list_remove (&ce->l_elem);
  hash_insert (&cache_hash, &ce->h_elem);
  list_push_front (&cache_list, &ce->l_elem);

  lock_release (&cache_lock);
  return ce;
}

struct cache_entry *
cache_get (block_sector_t sector_num)
{ 
  return add_to_cache (sector_num, false);
}

struct cache_entry *
cache_get_zeroed (block_sector_t sector_num)
{ 
  return add_to_cache (sector_num, true);
}

void 
cache_unpin (struct cache_entry *ce) 
{
  lock_acquire (&ce->lock);

  if (ce != NULL) 
    ce->pinned_cnt--;

  lock_release (&ce->lock);
}

void
cache_flush ()
{
  lock_acquire (&cache_lock);
  struct list_elem *e;
  for (e = list_begin (&cache_list); e != list_end (&cache_list);
       e = list_next (e)) {
    struct cache_entry *ce = list_entry (e, struct cache_entry, l_elem);
    if (ce->dirty)
      block_write (fs_device, ce->sector_num, ce->data);
  }
  lock_release (&cache_lock);
}

void
cache_destroy ()
{
  lock_acquire (&cache_lock);

  hash_destroy (&cache_hash, NULL);
  while (!list_empty (&cache_list)) {
    struct list_elem *e = list_pop_front (&cache_list);
    struct cache_entry *ce = list_entry (e, struct cache_entry, l_elem);
    if (ce->dirty)
      block_write (fs_device, ce->sector_num, ce->data);
    free (ce);
  }

  lock_release (&cache_lock);
}

/*
void
cache_read (struct cache_entry *ce, void *dest)
{
  lock_acquire (&ce->lock);
  block_read (fs_device, ce->sector_num, dest);
  lock_release (&ce->lock);
}

void cache_write (struct cache_entry *ce, void *dest)
{
  lock_acquire (&ce->lock);
  block_write (fs_device, ce->sector_num, dest);
  lock_release (&ce->lock);
}
*/
