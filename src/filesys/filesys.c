#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);

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

struct dir *get_lowest_dir (const char *path);

struct dir *
get_lowest_dir (const char *path)
{
  struct dir *upper_dir;
  if (strchr (path, '/') == path) { // if absolute pathname
    path++;
    upper_dir = dir_open_root ();
    if (upper_dir == NULL)
      PANIC ("open root dir failed");
  } else {                          // relative pathname
    upper_dir = dir_open (inode_open (thread_current ()->pwd));
    if (upper_dir == NULL)
      PANIC ("open process working directory failed");
  }

  if (strchr (path, '/') == NULL)
    return upper_dir;

  int path_len = strlen (path);
  if (path_len == 0)    // empty path
    return NULL;
  char path_cpy[path_len + 1];
  strlcpy (path_cpy, path, path_len + 1);
  
  struct dir *lower_dir;
  struct inode *lower_dir_inode;
  char *token, *save_ptr;
  for (token = strtok_r (path_cpy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr)) {
    if (strcmp (token, ".") == 0)
      continue;

    bool dir_exists = dir_lookup (upper_dir, token, &lower_dir_inode, true);
    dir_close (upper_dir);
    if (!dir_exists)
      return NULL;

    lower_dir = dir_open (lower_dir_inode);
    if (lower_dir == NULL)
      return NULL;
    upper_dir = lower_dir;

    if (strchr (save_ptr, '/') == NULL)
      break;
  }
  
  //if (*save_ptr == '\0')    // case where path ends in '/'
    //return NULL;

  return upper_dir; 
}

static const char *
get_filename (const char *path)
{
  const char *filename = strrchr (path, '/');
  if (filename == NULL) 
    return path;
  return filename + 1;
}

/* Creates a file from PATH with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size, bool is_dir) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = get_lowest_dir (path);
  if (dir == NULL)
    return false;
  const char *filename = get_filename (path);

  //printf ("filename: %s\n", filename);
  //printf ("dir addr: %p\n", dir);
  //printf ("dir sector: %d\n", inode_get_inumber (dir_get_inode(dir)));

  bool success;
  if (is_dir) {
    success = (dir != NULL && 
               free_map_allocate (1, &inode_sector) && 
               dir_create (inode_sector, 
                           inode_get_inumber (dir_get_inode (dir)),
                           initial_size) && 
               dir_add (dir, filename, inode_sector, is_dir));
  } else {
    success = (dir != NULL && 
               free_map_allocate (1, &inode_sector) && 
               inode_create (inode_sector, initial_size) &&
               dir_add (dir, filename, inode_sector, is_dir));
  }

  //printf ("finished adding dir\n");
  //printf ("success = %d\n", success);

  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given pathname PATH.
   Returns the new file if successful or a null pointer
   otherwise.
   if is_dir != NULL, tells caller if dir or not.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
void *
filesys_open (const char *path, bool *is_dir)
{
  if (strcmp (path, "/") == 0)
    return dir_open_root (); 

  struct dir *dir = get_lowest_dir (path);
  if (dir == NULL)
    return NULL;
  const char *filename = get_filename (path);
  struct inode *inode = NULL;

  bool dir_found, file_found;
  dir_found = dir_lookup (dir, filename, &inode, true);
  if (!dir_found)
    file_found = dir_lookup (dir, filename, &inode, false);
  dir_close (dir);

  if (dir_found) {
    if (is_dir != NULL) *is_dir = true;
    return dir_open (inode);
  }
  if (file_found) {
    if (is_dir != NULL) *is_dir = false;
    return file_open (inode);
  }
  return NULL;
}

/* Deletes the file at pathname PATH.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) 
{
  struct dir *dir = get_lowest_dir (path);
  if (dir == NULL)
    return false;
  const char *filename = get_filename (path);
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
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
