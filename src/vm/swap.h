#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"

#define SECTORS_PER_PAGE 8

void swap_init (void);
void swap_read_page (int swap_index, void *buffer); // read from swap
int swap_write_page (void *buffer);                 // write to swap
void swap_remove (int swap_index);

#endif
