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
  int waiting;
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
  while (true) {
    struct cache_entry dummy;
    dummy.sector_num = sector_num;
    struct hash_elem *hash_elem = hash_find (&cache_hash, &dummy.h_elem);

    if (hash_elem != NULL) {
      struct cache_entry *ce = hash_entry (hash_elem, struct cache_entry, h_elem);
      if (!lock_try_acquire(&ce->lock)) {
        ce->pinned_cnt++;
        lock_release (&cache_lock)
        lock_acquire (&ce->lock);
        lock_acquire (&cache_lock);
        ce->pinned_cnt--;
      } //After, always have cache lock & ce lock and orig. pin status
      ce->pinned_cnt++;
      list_remove (&ce->l_elem);
      list_push_front (&cache_list, &ce->l_elem);
      return ce;
    }
    //lock_acquire (&io_lock);
    struct io_entry *ioe == NULL;
    struct list_elem *e;
    for (e = list_begin (&io_list); e != list_end (&io_list);
         e = list_next (e)) {
     ioe = list_entry (e, struct io_entry, elem);
     if (ioe->sector_num == sector_num)
       break;
    }
    if (ioe == NULl || ioe->sector_num != sector_num) //Release I/O lock
      return NULL; 
     //went above : lock_release (&io_lock);
    ioe->waiters++;
    cond_wait (ioe->io_complete, &cache_lock);  // wait for IO to finish, problem here no cahce lock..
    ioe->waiters--;
    if (ioe_waiters == 0) {
      list_remove (ioe->elem);
      free (ioe);
    }
  } 
  return NULL; //Should not reach
}

static struct io_entry *
add_io_entry (block_sector_t sector_nume)
{
  struct io_entry *ioe = malloc (sizeof(io_entry));
  if (ioe == NULL)
    return NULL;
  ioe->sector_num = sector_num;
  ioe->waiting = 0;
  cond_init (&ioe->io_complete);
  list_push_back (&io_list, &ioe->elem);
  return ioe;
}

/* Not thread safe, assumes a lock is already head, cache lock*/
static struct cache_entry *
find_evict_entry (void) 
{
  struct list_elem *e;
  while (true) {
    for (e = list_end (&cache_list); e != list_begin (&cache_list); 
         e = list_prev (e)) {   // iterate backwards throguh list
      ce = list_entry (elem, struct cache_entry, l_elem);
      if (ce->pinned_cnt == 0)
        break;
    }   
    if (ce->pinned_cnt == 0) {
      break;
    }
  }
  return ce;
}

static struct cache_entry * 
add_to_cache (block_sector_t sector_num, bool zeroed)
{
  lock_acquire (&cache_lock);
  struct cache_entry *ce;
  block_sector_t old_sector_num;
  
  ce = cache_find (sector_num); // should only return once I/O completed

  if (ce != NULL) { //need to zero out
    if (zeroed)
      memset (ce->data, 0, BLOCK_SECTOR_SIZE);
      lock_release (&cache_lock);
    return ce;
  }

  if (cache_size >= MAX_CACHE_SIZE) {
    ASSERT (!list_empty (&cache_list));

    ce = find_evict_entry ();
    list_remove (&ce->l_elem);
    list_push_front (&cache_list, &ce->l_elem);
    hash_delete (&cache_hash, &ce->h_elem); 
    old_sector_num = ce->sector_num;
    ce->sector_num = sector_num;
    ce->dirty = false;
    ce->pinned_cnt = 1;
    hash_insert (&cache_hash, &ce->h_elem); //Add in with new value
 
    struct io_entry *evict_ioe = add_io_entry (old_sector_num);
    if (evict_ioe == NULL)
      return NULL; //What does recovery mean here

    lock_acquire (ce->lock); //No One can access until I/O complete

    lock_release (&cache_lock); // Release glob lock, same sector calls block

    if (ce->dirty) {
      block_write (fs_device, old_sector_num, ce->data);
      lock_acquire (&cache_lock);
      cond_broadcast (&evict_ioe->io_complete, &cache_lock); //Wake blocked thrds
      lock_release (&cache_lock);
    }

  } else {
    cache_size++;
    
    ce = malloc (sizeof(struct cache_entry));
    if (ce == NULL)
      return NULL;
    ce->dirty = false;
    ce->pinned_cnt = 1;
    ce->sector_num = sector_num;
    lock_init (&ce->lock);  
    lock_acquire (&ce->lock); //No one can access until I/O complete

    hash_insert (&cache_hash, &ce->h_elem); //Add in with new value
    list_push_front (&cache_list, &ce->l_elem);
    lock_release (&cache_lock);
  }
  
  if (zeroed)
    memset (ce->data, 0, BLOCK_SECTOR_SIZE);
  else
    block_read (fs_device, sector_num, ce->data);

  lock_release (&ce->lock)

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
