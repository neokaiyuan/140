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

static int cache_size;

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
  cache_size = 0;

  thread_create ("flush_thread", PRI_DEFAULT, &flush_func, NULL);
}

/* Note, not multi-threaded safe */
static struct cache_entry *
find (block_sector_t sector_num) 
{
  struct cache_entry dummy;
  dummy.sector_num = sector_num;
  struct hash_elem *hash_elem = hash_find (&cache_hash, &dummy.h_elem);

  if (hash_elem == NULL) 
    return NULL;

  struct cache_entry *ce = hash_entry (hash_elem, struct cache_entry, h_elem);

  return ce;
}

struct cache_entry *
cache_find (block_sector_t sector_num)
{
  lock_acquire (&cache_lock);

  struct cache_entry *ce = find (sector_num);

  if (ce != NULL){
    ce->pinned_cnt++;
    list_remove (&ce->l_elem);
    list_push_front (&cache_list, &ce->l_elem);
  }

  lock_release (&cache_lock);
  return ce;
}

void 
cache_unpin (struct cache_entry *ce) 
{
  lock_acquire (&ce->lock);

  if (ce != NULL) 
    ce->pinned_cnt--;

  lock_release (&ce->lock);
}

static struct cache_entry * 
add_to_cache (block_sector_t sector_num, bool zeroed)
{
  lock_acquire (&cache_lock);
  struct cache_entry *ce;

  if (cache_size  >= MAX_CACHE_SIZE) {
    ASSERT (!list_empty (&cache_list));

    struct list_elem *elem = list_pop_back (&cache_list);
    ce = list_entry (elem, struct cache_entry, l_elem);
    hash_delete (&cache_hash, &ce->h_elem);
    lock_release (&cache_lock); // NO NEED FINE LOCK HERE B/C OUT OF HASH/LIST

    if (ce->dirty)
      block_write (fs_device, ce->sector_num, ce->data);

  } else {
    cache_size++;
    lock_release (&cache_lock);

    ce = malloc (sizeof(struct cache_entry));
    if (ce == NULL)
      return NULL;
    lock_init (&ce->lock);  // MAY NEED THIS
  }
  
  if (zeroed)
    memset (ce->data, 0, BLOCK_SECTOR_SIZE);
  else
    block_read (fs_device, sector_num, ce->data);

  ce->sector_num = sector_num;
  ce->dirty = false;
  ce->pinned_cnt = 1;;
  
  lock_acquire (&cache_lock);

  hash_insert (&cache_hash, &ce->h_elem);
  list_push_front (&cache_list, &ce->l_elem);
  
  lock_release (&cache_lock);
  return ce;
}

struct cache_entry *
cache_add (block_sector_t sector_num)
{ 
  return add_to_cache (sector_num, false);
}

struct cache_entry *
cache_add_zeroed (block_sector_t sector_num)
{ 
  return add_to_cache (sector_num, true);
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
