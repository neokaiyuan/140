#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCKS 12
#define INDIRECT_BLOCKS 1
#define DBLY_INDIRECT_BLOCKS 1

static struct lock open_inode_lock;

//12 direct, 1 indirect, and 1 doubly indirect
/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct_blocks[DIRECT_BLOCKS];              /* Direct Blocks */
    block_sector_t indirect_block;                            /* Indirect block */
    block_sector_t dual_indirect_block;                       /* Dbly indirect block */
    off_t length;                                             /* File size in bytes. */
    unsigned magic;                                           /* Magic number. */
    uint32_t unused[112];                                     /* Not used. */
  };

static void free_inode_blocks (block_sector_t inode_sector);
static bool init_inode_blocks (block_sector_t inode_sector, size_t sectors);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted,false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock lock;                   /* Lock for open inodes */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */

static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos, int length) 
{
  
  ASSERT (inode != NULL);
  int indirect_block_size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);
  block_sector_t physical_sector;
  block_sector_t indirect_block_sector;
  block_sector_t dual_indirect_block_sector;

  if (pos < 0 || pos > length) {
    return -1;
  }

  int file_sec_num = pos / BLOCK_SECTOR_SIZE;

  if (file_sec_num < DIRECT_BLOCKS) {//Is a Direct Block
    /* Read in physical sector */
    cache_read_at (inode->sector, &physical_sector, file_sec_num 
                * sizeof (block_sector_t), sizeof (block_sector_t));
    if (physical_sector == 0)
      physical_sector = -1;
    return physical_sector;
  }
  
  if (file_sec_num < DIRECT_BLOCKS + indirect_block_size) { //Is in an indirect block
    int indirect_block_ofst = file_sec_num - DIRECT_BLOCKS;
    /* Read in indirect block sector */
    cache_read_at (inode->sector, &indirect_block_sector, DIRECT_BLOCKS 
                * sizeof (block_sector_t), sizeof (block_sector_t)); 
    /* Read in physical sector */
    cache_read_at (indirect_block_sector, &physical_sector, indirect_block_ofst 
                * sizeof (block_sector_t), sizeof (block_sector_t)); 
    if (physical_sector == 0)
      physical_sector = -1;
    return physical_sector;
  }

  file_sec_num -= (DIRECT_BLOCKS + INDIRECT_BLOCKS * indirect_block_size);

  int indirect_block_num = file_sec_num  / indirect_block_size; //Is in a dual indirect
  int indirect_block_ofst = file_sec_num % indirect_block_size;

  /* Read in doubly indirect sector */
  cache_read_at (inode->sector, &dual_indirect_block_sector, (DIRECT_BLOCKS + 
               INDIRECT_BLOCKS) * sizeof(block_sector_t), sizeof(block_sector_t));
  /* Read in indirect block sector */
  cache_read_at (dual_indirect_block_sector, &indirect_block_sector, indirect_block_num 
              * sizeof (block_sector_t), sizeof (block_sector_t));
  /* Read in physical sector */
  cache_read_at (indirect_block_sector, &physical_sector, indirect_block_ofst 
              * sizeof (block_sector_t), sizeof (block_sector_t));
  if (physical_sector == 0)
    physical_sector = -1;
  return physical_sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inode_lock);
}

/* Initialize an indirect block */
static bool
init_indirect_block (int indirect_sector, int sectors_left)
{
  int indirect_block_size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);
  cache_write_zeroed (indirect_sector);

  int i;
  bool success = true;
  block_sector_t sec_in_indirect;
  for (i = 0; i < indirect_block_size; i++ ) {
    success = free_map_allocate (1, &sec_in_indirect);
    if (!success)
      break;
    cache_write_at (indirect_sector, &sec_in_indirect, i * sizeof(block_sector_t),
                    sizeof (block_sector_t)); //Write to indirect block
    cache_write_zeroed (sec_in_indirect); //Add new sec to cache
    sectors_left--;
    if (sectors_left <= 0)
      break;
  }
  return success;
}

/* Initialize a doubly indirect block, called dual indirect */
static bool 
init_dual_indirect (block_sector_t inode_sector, int sectors_left)
{
  int indirect_block_size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);
  block_sector_t dual_indirect_block_sector;
  if (!free_map_allocate (1, &dual_indirect_block_sector)) {
    return false;
  }
  cache_write_at (inode_sector, &dual_indirect_block_sector, (DIRECT_BLOCKS + 
                   INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof(block_sector_t));   

  int num_indirect_blocks = sectors_left / indirect_block_size;
  if ( (sectors_left % indirect_block_size ) != 0) 
    num_indirect_blocks++; 

  ASSERT (num_indirect_blocks <= indirect_block_size);
  
  int i;
  bool success = true;
  block_sector_t indirect_in_dual_indirect;
  for (i = 0; i < num_indirect_blocks; i++) {
    success = free_map_allocate (1, &indirect_in_dual_indirect);
    if (!success)
      break;
    /* Write indirect sector number in doubly indirect block */
    cache_write_at (dual_indirect_block_sector, &indirect_in_dual_indirect, i *
                    sizeof (block_sector_t), sizeof (block_sector_t)); 
    /* Initalize the indirect block */
    success = init_indirect_block (indirect_in_dual_indirect, sectors_left);
    if (!success) 
      break;
    sectors_left -= indirect_block_size;
  }

  return success;
}

/* Finds sector for inode and zero them out */
static bool
init_inode_blocks (block_sector_t inode_sector, size_t sectors) {
  int sectors_left = sectors;
  int indirect_block_size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);
  block_sector_t physical_sector;
  block_sector_t indirect_block_sector;

  if (sectors_left > 0) { //Direct Blocks
    int i;  
    for (i = 0; i < DIRECT_BLOCKS; i++) {
      if (!free_map_allocate (1, &physical_sector))
        return false;
      cache_write_at (inode_sector, &physical_sector, i * 
                      sizeof (block_sector_t),  sizeof(block_sector_t));   
      sectors_left--;
      if (sectors_left == 0)
        break;
    }

  }

  if (sectors_left > 0) {  //Indirect Blocks
    if (!free_map_allocate (1, &indirect_block_sector)){
      return false;
    }
    cache_write_at (inode_sector, &indirect_block_sector, 
                    DIRECT_BLOCKS * sizeof (block_sector_t), sizeof(block_sector_t));   
    if (!init_indirect_block (indirect_block_sector, sectors_left)) {
      return false;
    }
    sectors_left -= indirect_block_size;
  }

  if (sectors_left > 0) {  //Doubly indirect blocks
    if (!init_dual_indirect (inode_sector, sectors_left)) {
      return false;
    }
  }

  return true; //No failures, return true
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */

bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = true;
  unsigned magic = INODE_MAGIC;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  size_t sectors = bytes_to_sectors (length);
  if (sectors == -1)
    return false;

  cache_write_zeroed (sector); //Init sector to 0

  cache_write_at (sector, &length, (DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (off_t));
  
  cache_write_at (sector, &magic, ((DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t)) + sizeof (off_t),
                     sizeof (off_t));

  
  success = init_inode_blocks (sector, sectors);

  if (!success) {
    free_inode_blocks(sector);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  lock_acquire (&open_inode_lock);

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {

          if (!lock_try_acquire(&inode->lock)) {
            lock_release (&open_inode_lock);
            lock_acquire (&inode->lock);
            lock_acquire (&open_inode_lock);
          }
      
          inode_reopen (inode);
          lock_release (&inode->lock);
          lock_release (&open_inode_lock);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  
  lock_init (&inode->lock);

  lock_release (&open_inode_lock);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  block_sector_t to_return = inode->sector;
  return to_return;
}

int
inode_get_open_cnt (const struct inode *inode)
{
  lock_acquire (&inode->lock);
  block_sector_t to_return = inode->open_cnt;
  lock_release (&inode->lock);
  return to_return;
}

static void
free_indirect_block (block_sector_t indirect_sector, int sectors_left) 
{
  if (indirect_sector == 0)
    return;
  int indirect_block_size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);
  block_sector_t sector_in_indirect;

  int i;
  for (i = 0; i < indirect_block_size; i++) {
    cache_read_at (indirect_sector, &sector_in_indirect, i * sizeof (block_sector_t),
                sizeof (block_sector_t)); //Read a sector number from cache
    if (sector_in_indirect == 0)
      break;
    free_map_release (sector_in_indirect, 1);
    sectors_left--; 
    if (sectors_left <= 0)
      break;
  }
}

static void 
free_dual_indirect (block_sector_t dual_indirect_sector, int sectors_left)  
{
  if (dual_indirect_sector == 0)
    return;
  int indirect_block_size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);

  int num_indirect_blocks = sectors_left / indirect_block_size;
  if ( (sectors_left % indirect_block_size ) != 0) 
    num_indirect_blocks++; 

  ASSERT (num_indirect_blocks <= indirect_block_size);
  
  int i;
  block_sector_t indirect_sector;
  for (i = 0; i < num_indirect_blocks; i++) {
    cache_read_at (dual_indirect_sector, &indirect_sector, i * sizeof (block_sector_t),
                sizeof (block_sector_t)); //Read an indirect sector number from cache
    if (indirect_sector == 0)
      break;
    free_indirect_block (indirect_sector, sectors_left);
    free_map_release (indirect_sector, 1);

    sectors_left -= indirect_block_size;
    if (sectors_left <= 0)
      break;
  }

  free_map_release (dual_indirect_sector, 1);
}

static void
free_inode_blocks (block_sector_t sector)
{
  off_t file_length;
  cache_read_at (sector, &file_length, (DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (off_t));
  int sectors_left  = bytes_to_sectors (file_length);
  int indirect_block_size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);
  block_sector_t physical_sector;
  block_sector_t indirect_block_sector;
  block_sector_t dual_indirect_block_sector;

  if (sectors_left > 0 ) { 

    int i;
    for (i = 0; i < DIRECT_BLOCKS; i++) {
      cache_read_at (sector, &physical_sector, i * sizeof (block_sector_t),
                    sizeof (block_sector_t)); 
      if (physical_sector == 0) { //In case this is an inode that failed to open 
        sectors_left = 0;
        break;
      }
      free_map_release (physical_sector, 1);  
      sectors_left--;
      if (sectors_left == 0)
        break;
    }

  }

  if (sectors_left > 0) {
    cache_read_at (sector, &indirect_block_sector, DIRECT_BLOCKS *
                sizeof (block_sector_t), sizeof (block_sector_t));

    if (indirect_block_sector == 0) {
      sectors_left = 0;
    } else {
    free_indirect_block (indirect_block_sector, sectors_left);
    sectors_left -= indirect_block_size;
    }

  }

  if (sectors_left > 0) {
    cache_read_at (sector, &dual_indirect_block_sector, (DIRECT_BLOCKS +
                INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (block_sector_t));

    if (dual_indirect_block_sector != 0)
      free_dual_indirect (dual_indirect_block_sector, sectors_left);
  }
  
}
/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */

void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_inode_blocks (inode->sector);
          free_map_release (inode->sector, 1);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  off_t file_length;
  cache_read_at (inode->sector, &file_length, (DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (off_t));

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset, file_length);
    if (sector_idx == -1) {
      return 0;
    }
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    bool success = true;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
      success = cache_read_at (sector_idx, buffer + bytes_read, 0, BLOCK_SECTOR_SIZE);
    else 
      success = cache_read_at (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);

    if (!success) 
      return 0;
      
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }

  return bytes_read;
}


bool
add_one_sector (struct inode *inode, int sec_index) 
{

  block_sector_t physical_sector;
  block_sector_t indirect_block_sector;
  block_sector_t dual_indirect_block_sector;
  int num_sectors_in_indirect = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);

  if (sec_index < DIRECT_BLOCKS) {

    if (!free_map_allocate (1, &physical_sector))
      return false;
    /* Write one sector to the direct block*/
    cache_write_at (inode->sector, &physical_sector, sec_index * 
                    sizeof (block_sector_t), sizeof (block_sector_t));
    return true;
  }

  if (sec_index < DIRECT_BLOCKS + num_sectors_in_indirect) {
    sec_index -= DIRECT_BLOCKS;
    int indirect_block_ofst = sec_index % num_sectors_in_indirect
                                * sizeof (block_sector_t);
    /* Retrieve indirect sector */
    if (indirect_block_ofst == 0) { //If not allocated, alloc
      if (!free_map_allocate (1, &indirect_block_sector)) 
        return false; 
      cache_write_at (inode->sector, &indirect_block_sector, DIRECT_BLOCKS *
                       sizeof (block_sector_t), sizeof (block_sector_t));
    } else {
      cache_read_at (inode->sector, &indirect_block_sector, DIRECT_BLOCKS *
                  sizeof (block_sector_t), sizeof (block_sector_t));
    }
    /* Get and write new sector in indirect block */
    free_map_allocate (1, &physical_sector);
    cache_write_at (indirect_block_sector, &physical_sector, indirect_block_ofst, 
                    sizeof (block_sector_t));

    return true;

  } else {

    sec_index -= (DIRECT_BLOCKS + num_sectors_in_indirect);

    /* Retrieve dual indirect sector*/
    cache_read_at (inode->sector, &dual_indirect_block_sector, (DIRECT_BLOCKS + 
                       INDIRECT_BLOCKS) * sizeof (block_sector_t), 
                        sizeof (block_sector_t));
    if (dual_indirect_block_sector == 0) { //If not allocated, allocate 
      if (!free_map_allocate (1, &dual_indirect_block_sector))
        return false;
      cache_write_at (inode->sector, &dual_indirect_block_sector, (DIRECT_BLOCKS + 
                       INDIRECT_BLOCKS) * sizeof (block_sector_t), 
                        sizeof (block_sector_t));
    } 

    int dual_indirect_ofst = sec_index / num_sectors_in_indirect 
                              * sizeof (block_sector_t); 
    int indirect_block_ofst = sec_index % num_sectors_in_indirect
                               * sizeof (block_sector_t);
    /* Retrieve Indirect block sector */
    if (indirect_block_ofst == 0) { //If first block of indirect, must alloc
      if(!free_map_allocate (1, &indirect_block_sector))
        return false;
      cache_write_at (dual_indirect_block_sector, &indirect_block_sector,
                        dual_indirect_ofst, sizeof (block_sector_t));
    } else {
      cache_read_at (dual_indirect_block_sector, &indirect_block_sector, 
                dual_indirect_ofst, sizeof (block_sector_t));
    }

    /* Get and write new sector into indirect block */
    if (!free_map_allocate (1, &physical_sector))
      return false;
    cache_write_at (indirect_block_sector, &physical_sector, indirect_block_ofst,
                    sizeof (block_sector_t));
  }
    return true;
}

bool
extend_inode (struct inode *inode, off_t pos, int write_size)
{
  int file_length;
  cache_read_at (inode->sector, &file_length, (DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (off_t));
  
  int max_sec_index = file_length / BLOCK_SECTOR_SIZE;   
  if (file_length == 0) { //When file is size 0
    max_sec_index = -1;
  }
  int write_sec_end = (pos + write_size) / BLOCK_SECTOR_SIZE;
  int num_sec_to_add = write_sec_end - max_sec_index;

  if (num_sec_to_add <= 0) {
    return false;
  } else {
    int i;
    for (i = 1; i <= num_sec_to_add; i++ ) {
      add_one_sector (inode, max_sec_index + i);
    }
  }
  return true;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  bool is_inode_extend = false;

  lock_acquire (&inode->lock);
  if (inode->deny_write_cnt) {
    lock_release (&inode->lock);
    return 0;
  }

  off_t file_length;
  cache_read_at (inode->sector, &file_length, (DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (off_t));

  if (offset + size > file_length) {
    is_inode_extend = true;
    extend_inode (inode, offset, size); 
    file_length = size + offset;
  } else {
    lock_release (&inode->lock);
  }


  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset, file_length);
    if (sector_idx == -1) {
      lock_release (&inode->lock);
      return 0;
    }

    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = file_length - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;
    bool success = true;


    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
      success = cache_write_at (sector_idx, buffer + bytes_written, 0, BLOCK_SECTOR_SIZE);
    else
      success = cache_write_at (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);

    if (!success) {
      lock_release (&inode->lock);
      return 0;
    }
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
 
  if (is_inode_extend) {
    /* Update file length of disk inode */
    cache_write_at (inode->sector, &file_length, (DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (off_t));
    lock_release (&inode->lock);
  } 

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{ 
  lock_acquire (&inode->lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release (&inode->lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  lock_acquire (&inode->lock);
  int inode_length;
  cache_read_at (inode->sector, &inode_length, (DIRECT_BLOCKS + INDIRECT_BLOCKS +
                   DBLY_INDIRECT_BLOCKS) * sizeof (block_sector_t), sizeof (off_t));
  lock_release (&inode->lock);
  return inode_length;
}
