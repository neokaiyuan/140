#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);

  lock_init(&filesys_lock);

  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  cache_destroy ();
}

static dir *
get_lowest_dir (const char *path)
{
  struct thread *t = thread_current ();
  const char *save_ptr = path;
  struct dir *upper_dir;
  if (strchr (path, '/') == path) { // if absolute pathname
    save_ptr++;
    upper_dir = dir_open_root ();
    if (upper_dir == NULL)
      PANIC ("open root dir failed");
  } else {                          // relative pathname
    upper_dir = dir_reopen (t->curr_dir);
    if (upper_dir == NULL)
      PANIC ("open process working directory failed");
  }

  int path_len = strlen (save_ptr);
  char path[path_len + 1];
  strlcpy (path, save_ptr, path_len + 1);
  
  struct dir *lower_dir;
  struct inode *lower_dir_inode;
  char *token;
  for (token = strtok_r (path, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr)) {
    if (strchr (save_ptr, '/') == NULL)
      break;
    if (strcmp (token, ".") == 0)
      continue;

    bool dir_exists = dir_lookup (upper_dir, token, &lower_dir_inode, true);
    dir_close (upper_dir);
    if (!dir_exists)
      return NULL;

    struct dir *lower_dir = dir_open (lower_dir_inode);
    if (lower_dir == NULL)
      return NULL;
    upper_dir = lower_dir;
  }

  return upper_dir; 
}

/* Creates a file from PATH with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = get_lowest_dir (path);
  if (dir == NULL)
    return false;
  char *filename = strrchr (path, '/') + 1;

  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, filename, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given pathname PATH.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *path)
{
  struct dir *dir = get_lowest_dir (path);
  if (dir == NULL)
    return NULL;
  char *filename = strrchr (path, '/') + 1;
  struct inode *inode = NULL;

  dir_lookup (dir, filename, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file at pathname PATH.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) 
{
  struct dir *dir = get_lowest_dir (path);
  char *filename = strrchr (path, '/') + 1;
  bool success = dir != NULL && dir_remove (dir, filename);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
