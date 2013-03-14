#include <hash.h>
#include <list.h>
#include <stdio.h>
#include <string.h>
#include "devices/block.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "filesys/filesys.h"

static struct list cache_list;
static struct hash cache_hash;
static struct lock cache_lock;
static struct list evict_list; // incoming I/Os taken care of by ce->lock
//static struct lock io_lock;
static int cache_size;

struct evict_entry {
  block_sector_t sector_num;
  struct list_elem elem;
  struct condition io_complete;
  int num_waiters;
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
  list_init (&evict_list);
  //lock_init (&io_lock);
  cache_size = 0;
  thread_create ("flush_thread", PRI_DEFAULT, &flush_func, NULL);
}

/* When return from this function, will always have cache lock.
   if ce was found, then will also return the ce * and the ce lock
   acquired. Otherwise, it returns NULL to indicate that the block
   sector number is not in the cache and is not being evicted from
   the cache */
static struct cache_entry *
cache_find (block_sector_t sector_num)
{
  while (true) { //Note, holding I/O lock on entrace
    struct cache_entry dummy;
    dummy.sector_num = sector_num;
    struct hash_elem *hash_elem = hash_find (&cache_hash, &dummy.h_elem);

    if (hash_elem != NULL) {
      struct cache_entry *ce = hash_entry (hash_elem, struct cache_entry, h_elem);
      if (!lock_try_acquire (&ce->lock)) { // To check if being brought in
        ce->pinned_cnt++;   // so doesn't get evicted when waiting for I/O
        lock_release (&cache_lock); // No race here because pinned = cannot evict
        lock_acquire (&ce->lock);   // wait on incoming or outgoing (cache_flush) I/O
        lock_acquire (&cache_lock);
        ce->pinned_cnt--;
      } // After, always have cache lock & ce lock and orig. pin status
      ce->pinned_cnt++; // so that there is no eviction between cache_get and memcpy
      list_remove (&ce->l_elem);
      list_push_front (&cache_list, &ce->l_elem);
      return ce;
    }

    struct evict_entry *ee = NULL;
    struct list_elem *e;
    for (e = list_begin (&evict_list); e != list_end (&evict_list);
         e = list_next (e)) {
     ee = list_entry (e, struct evict_entry, elem);
     if (ee->sector_num == sector_num)
       break;
    }
    if (ee == NULL || ee->sector_num != sector_num) // Not in cache or exiting 
      return NULL; 

    ee->num_waiters++;
    cond_wait (&ee->io_complete, &cache_lock);  // wait for outgoing IO to finish
    ee->num_waiters--;
    if (ee->num_waiters <= 0) {
      list_remove (&ee->elem);
      free (ee);
    }
  }
  return NULL; // Should not reach
}

static struct evict_entry *
add_evict_entry (block_sector_t sector_num)
{
  struct evict_entry *ee = malloc (sizeof(struct evict_entry));
  if (ee == NULL)
    return NULL;
  ee->sector_num = sector_num;
  ee->num_waiters = 0;
  cond_init (&ee->io_complete);
  list_push_back (&evict_list, &ee->elem);
  return ee;
}

/* Not thread safe, assumes a lock is already head: cache lock*/
struct cache_entry *
find_evict_cache_entry (void) 
{
  struct list_elem *e;
  struct cache_entry *ce;
  while (true) {
    for (e = list_back (&cache_list); e != list_head (&cache_list); 
         e = list_prev (e)) {   // iterate backwards through list
      ce = list_entry (e, struct cache_entry, l_elem);
      if (ce->pinned_cnt == 0)
        break;
    }   
    if (ce->pinned_cnt == 0) {
      break;
    }
  }
  return ce;
}

struct cache_entry * 
add_to_cache (block_sector_t sector_num, bool zeroed)
{
  lock_acquire (&cache_lock);
  
  struct cache_entry *ce = cache_find (sector_num); // should only return once I/O completed

  if (ce != NULL) { // may need to zero out
    lock_release (&cache_lock); // needs to be here in case ce == NULL
    if (zeroed)
      memset (ce->data, 0, BLOCK_SECTOR_SIZE);
    lock_release (&ce->lock); // ce is pinned and moved to list front in cache_find
    return ce;
  }

  if (cache_size >= MAX_CACHE_SIZE) {   // cache full
    struct evict_entry *ee;
    ASSERT (!list_empty (&cache_list));

    

    ce = find_evict_cache_entry (); // cache entry to evict
    lock_acquire (&ce->lock); // no one can access until in- and outbound I/O complete
    
    block_sector_t old_sector_num = ce->sector_num;
    bool old_dirty = ce->dirty;

    hash_delete (&cache_hash, &ce->h_elem);
    ce->sector_num = sector_num;
    ce->dirty = false;
    ce->pinned_cnt = 1;

    if (old_dirty) {
      ee = add_evict_entry (old_sector_num);
      if (ee == NULL) {
        lock_release (&cache_lock);
        return NULL; // What does recovery mean here? evict entry malloc fail
      }
    }

    list_remove (&ce->l_elem);
    list_push_front (&cache_list, &ce->l_elem);
    hash_insert (&cache_hash, &ce->h_elem);
    lock_release (&cache_lock); 

    if (old_dirty) {
      block_write (fs_device, old_sector_num, ce->data);  // holds old data temporarily
      lock_acquire (&cache_lock);
      if (ee->num_waiters == 0) {
        list_remove (&ee->elem);
        free (ee);
      } else {
        cond_broadcast (&ee->io_complete, &cache_lock); //Wake blocked threads
      }
      lock_release (&cache_lock);
    }

  } else {  // cache not full
    cache_size++;
    
    ce = malloc (sizeof(struct cache_entry));
    if (ce == NULL)
      return NULL;
    ce->sector_num = sector_num;
    ce->dirty = false;
    ce->pinned_cnt = 1; // default status is pinned, unpineed in cache_read/write
    lock_init (&ce->lock);  
    lock_acquire (&ce->lock); // No one can access until inbound I/O complete

    hash_insert (&cache_hash, &ce->h_elem); //Add in with new value
    list_push_front (&cache_list, &ce->l_elem);
    lock_release (&cache_lock);
  }
  
  if (zeroed)
    memset (ce->data, 0, BLOCK_SECTOR_SIZE);
  else
    block_read (fs_device, sector_num, ce->data);

  lock_release (&ce->lock);
  return ce;
}


static void 
cache_unpin (struct cache_entry *ce) 
{
  lock_acquire (&cache_lock);
  ce->pinned_cnt--;
  lock_release (&cache_lock);
}

bool
cache_read (block_sector_t sector_num, void *dest, int sector_ofs, 
           int chunk_size)
{ 
  struct cache_entry *ce = add_to_cache (sector_num, false);
  if (ce == NULL)
    return false; 
  memcpy (dest, ce->data + sector_ofs, chunk_size);
  cache_unpin (ce);
  return true;
}

bool
cache_write_at (block_sector_t sector_num, void *src, int sector_ofs, 
             int chunk_size)
{ 
  struct cache_entry *ce = add_to_cache (sector_num, false);
  if (ce == NULL)
    return false; 
  memcpy (ce->data + sector_ofs, src, chunk_size);
  ce->dirty = true;
  cache_unpin (ce);
  return true;
}

bool
cache_write_zeroed (block_sector_t sector_num)
{ 
  struct cache_entry *ce = add_to_cache (sector_num, true);
  if (ce == NULL)
    return false;
  cache_unpin (ce);
  return true;
}

void
cache_flush ()
{
  lock_acquire (&cache_lock);
  struct list_elem *e;
  for (e = list_begin (&cache_list); e != list_end (&cache_list);
       e = list_next (e)) {
    if (e == list_tail (&cache_list)) 
      return;
    struct cache_entry *ce = list_entry (e, struct cache_entry, l_elem);
    if (lock_try_acquire (&ce->lock) && ce->dirty) {
      lock_release (&cache_lock);
      block_write (fs_device, ce->sector_num, ce->data);
      ce->dirty = false;
      ce->pinned_cnt++;   // prevent eviction from moving this to front
      lock_release (&ce->lock);
      lock_acquire (&cache_lock);
      ce->pinned_cnt--;
    }
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
