#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);
static bool mem_valid (const void *ptr, int size);
static bool str_valid (const char *ptr);
static void *get_arg_n (int arg_num, void *esp);
static void halt (void) NO_RETURN;
static void exit (int status) NO_RETURN;
static pid_t exec (const char *file);
static int wait (pid_t);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static int filesize (int fd);
static int read (int fd, void *buffer, unsigned length);
static int write (int fd, const void *buffer, unsigned length);
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);
static bool chdir (const char *dir);
static bool mkdir (const char *dir);
static bool readdir (int fd, char *name);
static bool isdir (int fd);
static int inumber (int fd);

#define MAX_WRITE_SIZE 500

struct inode;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  void *esp = f->esp;
  if (!mem_valid ((const void *) esp, sizeof(int))) { 
    exit(-1);
  }

  int status;
  int syscall_num = *(int *) esp;  
  switch (syscall_num) {
    case SYS_HALT: 
      halt ();
      break;
    case SYS_EXIT:
      status = *(int *) get_arg_n(1, esp);
      f->eax = status;
      exit (status);
      break;
    case SYS_EXEC:
      f->eax = exec (*(char **) get_arg_n(1, esp));
      break;
    case SYS_WAIT:
      f->eax = wait (*(pid_t *) get_arg_n(1, esp));
      break;
    case SYS_CREATE:
      f->eax = (int) create (*(char **) get_arg_n(1, esp), 
                                        *(unsigned *) get_arg_n(2, esp));
      break;
    case SYS_REMOVE:
      f->eax = (int) remove (*(char **) get_arg_n(1, esp)); 
      break; 
    case SYS_OPEN:
      f->eax = open (*(char **) get_arg_n(1, esp));
      break;
    case SYS_FILESIZE:
      f->eax = filesize (*(int *) get_arg_n(1, esp));
      break;
    case SYS_READ:
      f->eax = read (*(int *) get_arg_n (1, esp), *(void **) get_arg_n(2, esp), 
                     *(unsigned *) get_arg_n(3, esp));
      break;
    case SYS_WRITE:
      f->eax = write (*(int *) get_arg_n(1, esp), *(void **) get_arg_n(2, esp), 
                      *(unsigned *) get_arg_n(3, esp));
      break;
    case SYS_SEEK:
      seek (*(int *) get_arg_n(1, esp), *(unsigned *) get_arg_n(2, esp));
      break;
    case SYS_TELL:
      f->eax = tell (*(int *) get_arg_n(1, esp));
      break;
    case SYS_CLOSE:
      close (*(int *) get_arg_n(1, esp));
      break;
    case SYS_CHDIR:
      f->eax = (int) chdir (*(char **) get_arg_n (1, esp));
      break;
    case SYS_MKDIR:
      f->eax = (int) mkdir (*(char **) get_arg_n (1, esp));
      break;
    case SYS_READDIR:
      f->eax = (int) readdir (*(int *) get_arg_n (1, esp), 
                              *(char **) get_arg_n (2, esp));
      break;
    case SYS_ISDIR:
      f->eax = (int) isdir (*(int *) get_arg_n (1, esp));
      break;
    case SYS_INUMBER:
      f->eax = inumber (*(int *) get_arg_n (1, esp));
      break;
    default:
      ASSERT (false);
      break;  
  }
}

static bool
mem_valid (const void *ptr, int size) 
{
  if (ptr == NULL || ptr >= PHYS_BASE) 
    return false;

  void *last_byte = (void *) ((unsigned) ptr + size)-1;
  if (last_byte >= PHYS_BASE)
    return false;

  int page_num_first_byte = (unsigned) ptr / PGSIZE;
  int page_num_last_byte = (unsigned) last_byte / PGSIZE;
  int pages_in_between = page_num_last_byte - page_num_first_byte;
  const void *page_of_memory = ptr; 

  int i;
  for (i = 0; i < pages_in_between+1; i++) {
    if (pagedir_get_page (thread_current()->pagedir, page_of_memory) == NULL)
      return false;
    page_of_memory += PGSIZE;
  }

  return true;
}

static bool 
str_valid (const char *ptr) 
{
  if (ptr == (char *) NULL || ptr >= (char *) PHYS_BASE || 
      pagedir_get_page(thread_current()->pagedir, ptr) == NULL)
    return false;

  const char *curr_byte_ptr = ptr;
  while (true) { 
    if (*curr_byte_ptr == '\0') return true;

    curr_byte_ptr += sizeof(char);

    if ((unsigned) curr_byte_ptr % PGSIZE == 0 &&
        (curr_byte_ptr >= (char *) PHYS_BASE || 
        pagedir_get_page (thread_current()->pagedir, curr_byte_ptr) == NULL)) 
      return false;
  }
  return false;   // should never get here
}

static void *
get_arg_n (int arg_num, void *esp) {
  void *arg_addr = esp + sizeof(char *) * arg_num;
  if (!mem_valid (arg_addr, sizeof(char *)))
    exit(-1);
  return arg_addr;
}

static void
halt (void)
{
  shutdown_power_off();
}

static void
exit (int status)
{
  thread_current ()->exit_status = status;
  thread_exit ();
}

static pid_t
exec (const char *path)
{
  if (!str_valid (path)) 
    exit(-1);

  struct thread *t = thread_current();
  pid_t pid = (pid_t) process_execute (path);
  if (pid == TID_ERROR) return -1;
  sema_down (&t->child_exec_sema); // look at start_process
  if (!t->child_exec_success) return -1;
  return pid; 
}

static int
wait (pid_t pid)
{
  return process_wait ((tid_t) pid);
}

static bool
create (const char *path, unsigned initial_size)
{
  if (!str_valid(path)) 
    exit(-1);

  return filesys_create (path, initial_size, false);
}

static bool
remove (const char *path)
{
  if (!str_valid(path)) 
    return false;

  return filesys_remove (path);
}

static int
open (const char *path)
{
  if (!str_valid(path)) 
    exit(-1);

  struct thread *t = thread_current ();

  bool is_dir;
  struct fd_entry *fde = malloc (sizeof (struct fd_entry));
  if (fde == NULL)
    return -1;
  fde->fd = t->next_open_fd; 
  fde->file = filesys_open (path, &is_dir);
  if (fde->file == NULL)
    return -1;
  fde->is_dir = is_dir;
  list_push_back (&t->fd_list, &fde->l_elem);
  hash_insert (&t->fd_hash, &fde->h_elem);

  t->next_open_fd++; 
  t->num_open_files++;
  return fde->fd;
}

static struct fd_entry *
get_fd_entry (int fd)
{
  struct thread *t = thread_current ();
  struct fd_entry dummy;
  dummy.fd = fd;
  struct hash_elem *he = hash_find (&t->fd_hash, &dummy.h_elem);
  if (he == NULL)
    exit(-1);
  return hash_entry (he, struct fd_entry, h_elem);
}

static int
filesize (int fd)
{
  if (fd == 0 || fd == 1)
    exit(-1);

  struct fd_entry *fde = get_fd_entry (fd);
  if (fde->is_dir)
    exit(-1);
  return file_length ((struct file *) fde->file);
}

static int
read (int fd, void *buffer, unsigned length)
{
  if (fd == 1 || !mem_valid(buffer, length)) 
    exit(-1);

  if (length == 0)
    return 0;

  if (fd == 0) {
    int read_len = 0;
    char c;
    while ((unsigned) read_len != length) {
      c = input_getc();
      memcpy (buffer + read_len, &c, 1);
      read_len++;
    }
    return read_len;
  }

  struct fd_entry *fde = get_fd_entry (fd);
  if (fde->is_dir)
    return -1;
  return file_read ((struct file *) fde->file, buffer, length);
}

static int
write (int fd, const void *buffer, unsigned length)
{
  if (fd == 0 || !mem_valid(buffer, length))
    exit(-1);

  if (length == 0) 
    return 0;

  if (fd == 1) {
    int num_writes = length / MAX_WRITE_SIZE;
    if (length % MAX_WRITE_SIZE > 0)
      num_writes++;
    while (num_writes > 0) {
      if (num_writes == 1) { 
        putbuf (buffer, length % MAX_WRITE_SIZE);
      } else {
        putbuf (buffer, MAX_WRITE_SIZE);
        buffer += MAX_WRITE_SIZE;
      }
      num_writes--;
    }
    return length;
  }
  
  struct fd_entry *fde = get_fd_entry (fd);
  if (fde->is_dir)
    return -1;
  return file_write ((struct file *) fde->file, buffer, length);
}

static void
seek (int fd, unsigned position)
{
  if (fd == 0 || fd == 1)
    exit(-1); 
  
  struct fd_entry *fde = get_fd_entry (fd);
  if (fde->is_dir)
    return;
  file_seek ((struct file *) fde->file, position);
}

static unsigned
tell (int fd)
{
  if (fd == 0 || fd == 1)
    exit(-1); 
  
  struct fd_entry *fde = get_fd_entry (fd);
  if (fde->is_dir)
    exit(-1);
  return file_tell ((struct file *) fde->file);
}

static void
close (int fd)
{
  if (fd == 0 || fd == 1)
     return; 

  struct fd_entry *fde = get_fd_entry (fd);
  if (fde->is_dir)
    dir_close ((struct dir *) fde->file);
  else
    file_close ((struct file *) fde->file);

  list_remove (&fde->l_elem);
  hash_delete (&thread_current ()->fd_hash, &fde->h_elem); 
}

static bool 
chdir (const char *dir)
{
  if (!str_valid (dir))
    exit(-1);

  bool is_dir;
  struct dir *new_dir = filesys_open (dir, &is_dir);
  if (new_dir == NULL || !is_dir)
    return false;

  thread_current ()->pwd = inode_get_inumber (dir_get_inode (new_dir));
  dir_close (new_dir);
  return true;
}

static bool 
mkdir (const char *dir)
{
  if (!str_valid (dir))
    exit(-1);

  //printf ("inside mkdir\n");

  if (!filesys_create (dir, 0, true)) {
    //printf ("mkdir failure\n");
    return false;
  }  

  //printf ("mkdir success\n");
  return true;
}

static bool 
readdir (int fd, char *name)
{
  if (!mem_valid (name, NAME_MAX + 1))
    exit(-1);

  struct fd_entry *fde = get_fd_entry (fd);
  if (!fde->is_dir)
    return false;
  return dir_readdir ((struct dir *) fde->file, name);
}

static bool 
isdir (int fd)
{
  struct fd_entry *fde = get_fd_entry (fd);
  return fde->is_dir;
}

static int 
inumber (int fd)
{
  struct fd_entry *fde = get_fd_entry (fd);
  struct inode *inode;
  if (fde->is_dir) {
    struct dir *dir = (struct dir *) fde->file;
    inode = dir_get_inode (dir);
  } else {
    struct file *file = (struct file *) fde->file;
    inode = file_get_inode (file);
  }
  return inode_get_inumber (inode);
}
