#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>

#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/string.h"
#include "threads/interrupt.h"
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

#define MAX_WRITE_SIZE 500

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
    /* Above checking size of int because esp is pointer to syscall num */
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

static bool str_valid(const char *ptr){
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
  struct thread *t = thread_current();
  t->exit_status = status;
  thread_exit ();
}

static pid_t
exec (const char *file)
{
  if (!str_valid (file)) 
    exit(-1);

  struct thread *t = thread_current();
  pid_t pid = (pid_t) process_execute (file);
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
create (const char *file, unsigned initial_size)
{
  if (!str_valid(file)) 
    exit(-1);
  lock_acquire(&filesys_lock);
  bool create = filesys_create (file, initial_size);
  lock_release(&filesys_lock);
  return create;
}

static bool
remove (const char *file)
{
  if (!str_valid(file)) 
    return false;
  lock_acquire(&filesys_lock);
  bool remove = filesys_remove (file);
  lock_release(&filesys_lock);
  return remove;
}

static int
open (const char *file)
{
  if (!str_valid(file)) 
    exit(-1);
  struct thread *t = thread_current ();
  if (t->next_open_file_index == MAX_FD_INDEX+1) 
    return -1;
  int new_fd = t->next_open_file_index;

  lock_acquire(&filesys_lock);
  t->file_ptrs[new_fd] = filesys_open (file);
  lock_release(&filesys_lock);

  if (t->file_ptrs[new_fd] == NULL)
    return -1;

  int new_index = 2;
  while (t->file_ptrs[new_index] != NULL && new_index != MAX_FD_INDEX)
    new_index++;
  if (new_index == MAX_FD_INDEX) 
    t->next_open_file_index = MAX_FD_INDEX+1; //Sentinel for no free fd
  else 
    t->next_open_file_index = new_index; 

  return new_fd;
}

static int
filesize (int fd)
{
  struct thread *t = thread_current ();
  if (fd == 0 || fd == 1 || fd >= MAX_FD_INDEX + 1 || t->file_ptrs[fd] == NULL)
    exit(-1);

  lock_acquire(&filesys_lock);
  int length = file_length (thread_current ()->file_ptrs[fd]);
  lock_release(&filesys_lock);
  return length;
}

static int
read (int fd, void *buffer, unsigned length)
{
  struct thread *t = thread_current ();
  if (fd == 1 || fd >= MAX_FD_INDEX+1 || 
      (t->file_ptrs[fd] == NULL && fd != 0))
    exit(-1);

  if (!mem_valid(buffer, length)) 
    exit(-1);

  if (length == 0)
    return 0;

  if (fd == 0) {
    int index = 0;
    char c;
    while ((unsigned) index != length) {
      c = input_getc();
      memcpy (buffer + index, &c, 1);
      index++;
    }    
    return index;
  }

  lock_acquire(&filesys_lock);
  int read_len = file_read (t->file_ptrs[fd], buffer, length);
  lock_release(&filesys_lock);
  return read_len;
}

static int
write (int fd, const void *buffer, unsigned length)
{
  struct thread *t = thread_current();
  if (fd == 0 || fd >=  MAX_FD_INDEX+1 ||
      (t->file_ptrs[fd] == NULL && fd != 1))
    exit(-1);   
  
  if (!mem_valid(buffer, length))
    exit(-1);

  if (length == 0) 
    return 0;

  if (fd == 1) {
    int num_writes = length / MAX_WRITE_SIZE + 1;
    const void *src = buffer;
    while (num_writes > 0) {
      if (num_writes == 1) { 
        putbuf (src, length % MAX_WRITE_SIZE);
      } else {
        putbuf(src, MAX_WRITE_SIZE);
        src += MAX_WRITE_SIZE;
      }
      num_writes--;
    }
    return length;
  } 
  
  lock_acquire(&filesys_lock);
  int write_len = file_write (t->file_ptrs[fd], buffer, length);
  lock_release(&filesys_lock);
  return write_len;
}

static void
seek (int fd, unsigned position)
{
  struct thread *t = thread_current ();
  if (fd == 0 || fd == 1 || fd >= MAX_FD_INDEX + 1|| t->file_ptrs[fd] == NULL)
    exit(-1); 
  
  lock_acquire(&filesys_lock); 
  file_seek (t->file_ptrs[fd], position);
  lock_release(&filesys_lock);
}

static unsigned
tell (int fd)
{
  struct thread *t = thread_current ();
  if (fd == 0 || fd == 1 || fd >= MAX_FD_INDEX + 1 || t->file_ptrs[fd] == NULL)
     exit(-1); 
  
  lock_acquire(&filesys_lock);
  int position = file_tell(t->file_ptrs[fd]);
  lock_release(&filesys_lock);
  return position;
}

static void
close (int fd)
{
  struct thread *t = thread_current ();
  if (fd == 0 || fd == 1 || fd >= MAX_FD_INDEX + 1 || t->file_ptrs[fd] == NULL)
     return; 

  lock_acquire(&filesys_lock);
  file_close (t->file_ptrs[fd]);
  lock_release(&filesys_lock);

  t->file_ptrs[fd] = NULL;
  t->next_open_file_index = fd;
}


