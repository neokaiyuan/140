#include <stdio.h>
#include "vm/page.h"





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

void
page_add (struct hash *sup_page_table, void *upage, void *kpage,
          enum page_type, enum page_loc, int swap_index, struct file *file,
          int file_offset, bool zeroed, bool writeable)
{
  struct sup_page_entry *entry = malloc (sizeof(struct sup_page_entry));
  entry->upage = upage;
  entry->kpage = kpage;
  entry->page_type = page_type;
  entry->page_loc = page_loc;
  entry->swap_index = swap_index;
  entry->file = file;
  entry->file_offset = file_offset;
  entry->zeroed = zeroed;
  entry->writable = writable;

  hash_insert (sup_page_table, &entry->elem);
}

void
page_remove (struct hash *sup_page_table, void *upage) 
{
  struct sup_page_entry dummy;
  dummy->upage = upage;
  struct sup_page_entry *entry_to_remove = hash_find (sup_page_table, 
                                                     &dummy.elem);



  free (entry_to_remove);
}
