#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCKS 12
#define INDIRECT_BLOCKS 1
#define DBLY_INDIRECT_BLOCKS 1


static void free_inode_blocks (struct inode_disk *disk_inode);

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
    uint32_t unused[125];                                     /* Not used. */
  };

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
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */

//Devon -- Here add conversion to allow the correct sector number to be found
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initialize an indirect block */
static bool
init_indirect_block (int sector_num, int sectors_left)
{
  static char zeros[BLOCK_SECTOR_SIZE];
  int indirect_block_size = BLOCK_SECTOR_SIZE/sizeof (block_sector_t);
  static block_sector_t indirect_block_cpy[indirect_block_size];
  for (int i = 0; i < indirect_block_size; i++ ) {
    if (!free_map_allocate (1, &indirect_block_cpy[i]));
      return false;
    block_write (fs_device, indirect_block_cpy[i], zeros);
    sectors_left--;
    if (sectors_left <= 0)
      break;
    }
    block_write (fs_device,  sector_num, indirect_block_cpy);
    return true;
}

/* Initialize a doubly indirect block, called dual indirect */
static bool 
init_dual_indirect (struct inode_disk *disk_inode, int sectors_left)
{
  int indirect_block_size = BLOCK_SECTOR_SIZE/sizeof (block_sector_t);
  static block_sector_t dual_indirect_cpy[indirect_block_size];

  if (!free_map_allocate (1, &disk_inode->dual_indirect_block))
    return false;

  int num_indirect_blocks = sectors_left / indirect_block_size;
  if ( (sectors_left % indirect_block_size ) != 0) 
    num_indirect_blocks++; 

  ASSERT (num_indirect_blocks <= indirect_block_size);

  for (int i = 0; i < num_indirect_blocks; i++) {
    if(!free_map_allocate (1, &dual_indirect_cpy[i]))
      return false;
    init_indirect_block (dual_indirect_cpy[i]);
  }
  block_write (fs_device, &disk_inode->dual_direct_block, dual_indirect_cpy);
  return true;
}

/* Finds sector for inode and zero them out */
static bool
init_inode_blocks (struct inode_disk *disk_inode, size_t sectors) {
  bool success = false;
  int sectors_left = sectors;
  int indirect_block_size = BLOCK_SECTOR_SIZE/sizeof (block_sector_t);
  static char zeros[BLOCK_SECTOR_SIZE];

  if (free_map_allocate (1, &disk_inode->direct_blocks[0])) {//FIX, gives first block
    success = true;
    if (sectors_left > 0) {
      for (int i = 0; i < DIRECT_BLOCKS; i++) {
        if (!free_map_allocate (1, &disk_inode->direct_blocks[i]));
          return false;
        block_write (fs_device, disk_inode->direct_blocks[i], zeros);
        sectors_left--;
        if (sectors_left <= 0)
          break;
      }
    }
    if (sectors_left > 0) {
      if (!free_map_allocate (1, &disk_inode->indirect_block))
        return false;
      if (!init_indirect_block (disk_inode->indirect_block, sectors_left));
        return false;
      sectors_left -= indirect_block_size;;
    }
    if (sectors_left > 0) {
      if (!init_dual_indirect (disk_inode, sectors_left));
        return false;
    }
  }
  return success;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */

//Devon -- Here must actually choose the sectors on disk to allocate and then zero them out
//First sector is given in inode
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length); //FIX
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      
      success = init_inode_blocks (disk_inode, sectors);
      if (success)
        block_write (fs_device, sector, disk_inode);
      else
        free_inode_blocks (disk_inode);
      free (disk_inode);
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

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
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
  block_read (fs_device, inode->sector, &inode->data);
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
  return inode->sector;
}

static void
free_indirect_block (block_sector_t sec_num, int num_sectors) 
{
  int indirect_block_size = BLOCK_SECTOR_SIZE/sizeof (block_sector_t);
  static block_sector_t indirect_block_cpy[indirect_block_size];
  block_read (fs_device, sec_num, indirect_block_cpy); //Think about nubmer being null
  for (int i = 0; i < indirect_block_size; i++) {
    if (indirect_block_cpy[i] == 0)
      break;
    free_map_release (indirect_block_cpy[i], 1);(
    num_sectors--; 
    if (num_sectors <= 0)
      break;
  }
}

static void 
free_dual_indirect (block_sector_t sec_num, int num_sectors)  
{
  int indirect_block_size = BLOCK_SECTOR_SIZE/sizeof (block_sector_t);
  static block_sector_t dual_indirect_cpy[indirect_block_size];

  int num_indirect_blocks = sectors_left / indirect_block_size;
  if ( (sectors_left % indirect_block_size ) != 0) 
    num_indirect_blocks++; 

  ASSERT (num_indirect_blocks <= indirect_block_size);
  block_read (fs_device, sec_num, dual_indirect_cpy);

  for (int i = 0; i < num_indirect_block; i++) {
    if (dual_indirect_cpy[i] == 0)
      break;
    map_release (dual_indirect_cpy[i], 1);
  }

  map_release (sec_num, 1);

}

static void
free_inode_blocks (struct inode_disk *disk_inode)
{
  int num_sectors = bytes_to_sectors (inode->data.length);
  int indirect_block_size = BLOCK_SECTOR_SIZE/sizeof (block_sector_t);

  if (num_sectors > 0 ) { 
    for (int i = 0; i < DIRECT_BLOCKS; i++) {
      if (inode->direct_blocks[i] == 0) { //In case this is an inode that failed to open 
        num_sectors = 0;
        break;
      }
      free_map_release (inode->direct_blocks[i], 1);  
      num_sectors--;
      if (num_sectors == 0)
        break;
    }
  }

  if (num_sectors > 0) {
    if (inode->indirect_block == 0)
      break;
    free_indirect_block (inode->indirect_block, num_sectors);
    num_sectors -= indirect_block_size;
  }

  if (num_sectors > 0) {
    if (inode->dual_indirect_block == 0)
      break;
    free_dual_indirect (inode->dual_indirect_block, num_sectors);
  }
  
}
/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */

//Devon, need to change this to remove the correct sector numbers
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
          free_inode_blocks (&inode->data);
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

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    struct cache_entry *ce = cache_find (sector_idx);
      if (ce == NULL)
        ce = cache_add (sector_idx);
      if (ce == NULL) 
        return 0;
    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
      memcpy (buffer + bytes_read, ce->data, BLOCK_SECTOR_SIZE); 
    else 
      memcpy (buffer + bytes_read, ce->data + sector_ofs, chunk_size);
      
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }

  return bytes_read;
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

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    struct cache_entry *ce = cache_find (sector_idx);
    if (ce == NULL)
      ce = cache_add (sector_idx);
    if (ce == NULL)
      return 0;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
      memcpy (ce->data, buffer + bytes_written, BLOCK_SECTOR_SIZE);
    else
      memcpy (ce->data + sector_ofs, buffer + bytes_written, chunk_size);

    ce->dirty = true;

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
