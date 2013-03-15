#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <hash.h>
#include "devices/block.h"
#include "threads/synch.h"

#define BLOCK_SECTOR_SIZE 512
#define MAX_CACHE_SIZE 64

struct cache_entry {
  block_sector_t sector_num;
  struct list_elem l_elem;
  struct hash_elem h_elem;
  struct lock lock;
  bool dirty;
  bool pinned_cnt;
  char data[BLOCK_SECTOR_SIZE];
};

/*//READ AHEAD
struct read_ahead_entry {
  block_sector_t sector_num;
  struct list_elem elem;
};

bool cache_read (block_sector_t sector_num, int next_sector,
                            void *dest, int sector_ofs, int chunk_size);

void cache_read_ahead (void);

*/
void cache_init (void);

bool cache_read (block_sector_t sector_num, void *dest, int sector_ofs,
                 int chunk_size);
bool cache_write_at (block_sector_t sector_num, const void *src, int sector_ofs,
                  int chunk_size);
bool cache_write_zeroed (block_sector_t sector_num);

//void cache_unpin (struct cache_entry *ce);
void cache_flush (void);
void cache_destroy (void);


#endif
