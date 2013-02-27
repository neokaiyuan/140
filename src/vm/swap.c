#include "vm/swap.h"
#include <bitmap.h>
#include <stdio.h>
#include "devices/block.h"
#include "threads/synch.h"

struct bitmap *swap_table;
struct lock swap_table_lock;

void 
swap_init ()
{
  int size_in_pages = block_size (block_get_role (BLOCK_SWAP)) / SECTORS_PER_PAGE;
  swap_table = bitmap_create (size_in_pages);
  ASSERT (swap_table != NULL);
  lock_init (&swap_table_lock);
}

void
swap_read_page (int swap_index, void *buffer)
{
  struct block *swap_block = block_get_role (BLOCK_SWAP);

  int i;
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_read (swap_block, swap_index * SECTORS_PER_PAGE + i,
                buffer + i * BLOCK_SECTOR_SIZE);
  }

  bitmap_set (swap_table, swap_index, false);
}

int
swap_write_page (void *buffer)
{
  struct block *swap_block = block_get_role (BLOCK_SWAP);
  
  lock_acquire (&swap_table_lock);
  int swap_index = bitmap_scan_and_flip (swap_table, 0, 1, false);
  lock_release (&swap_table_lock);
  ASSERT (swap_index != BITMAP_ERROR);

  int i;
  for (i = 0; i < SECTORS_PER_PAGE; i++) {
    block_write (swap_block, swap_index * SECTORS_PER_PAGE + i,
                 buffer + i * BLOCK_SECTOR_SIZE);
  }

  return swap_index;
}

void 
swap_remove (int swap_index)
{
  bitmap_set (swap_table, swap_index, false);
}
