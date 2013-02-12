#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "lib/string.h"
#include "devices/shutdown.h"

static void syscall_handler (struct intr_frame *);
static bool addr_valid (void *ptr);
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

#define MAX_SINGLE_WRITE 300
struct lock filesys_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  void *esp = f->esp;
  if (!addr_valid (esp)) {
    //Stop process
    //Free Assocated memory
    //Below, not thread_exit, instead call our function
    thread_exit();
  }

  int syscall_num = *(int *) esp;  
  switch (syscall_num) {
    case SYS_HALT: 
      halt ();
      break;
    case SYS_EXIT:
      int status = *(int *) get_arg_n(1, esp);
      f->eax = status;
      exit (status);
      break;
    case SYS_EXEC:
      (pid_t) f->eax = exec (*(char **) get_arg_n(1, esp));
      break;
    case SYS_WAIT:
      f->eax = wait (*(pid_t *) get_arg_n(1, esp));
      break;
    case SYS_CREATE:
      (bool) f->eax = create (*(char **) get_arg_n(1, esp), 
                                        *(unsigned *) get_arg_n(2, esp));
      break;
    case SYS_REMOVE:
      (bool) f->eax = remove (*(char **) get_arg_n(1, esp)); 
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
      (unsigned) f->eax = tell (*(int *) get_arg_n(1, esp));
      break;
    case SYS_CLOSE:
      close (*(int *) get_arg_n(1, esp));
      break;
    default:
      ASSERT (false);
      break;  
  }

  printf ("system call!\n");
  thread_exit ();
}

static bool
addr_valid (void *ptr) 
{
  if ((int) ptr < 0 || ptr >= PHYS_BASE) return false;
  // MAY WANT TO PUT THIS PROTOTYPE IN userprog/pagedir.h
  if (pagedir_get_page (thread_current()->pagedir, ptr) == NULL) 
    return false;
  return true;
}

static void *
get_arg_n (int arg_num, void *esp) {
  void *arg_addr = esp + sizeof(char *) * arg_num;
  ASSERT (addr_valid(arg_addr));
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
  printf("%s: exit(%d)\n", t->name, status);
  thread_exit ();
}

static pid_t
exec (const char *file)
{
  pid_t pid = (pid_t) process_execute (file);
  if (pid == TID_ERROR) return -1;
  sema_down (&thread_current()->child_exec_sema); // look at start_process
  if (!thread_current()->child_exec_success) return -1;
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
  lock_acquire(&filesys_lock);
  bool create = filesys_create (file, initial_size);
  lock_release(&filesys_lock);
  return create;
}

static bool
remove (const char *file)
{
  lock_acquire(&filesys_lock);
  bool remove = filesys_remove (file);
  lock_release(&filesys_lock);
  return remove;
}

static int
open (const char *file)
{
  struct thread *t = thread_current ();
  if (t->next_open_file_index == MAX_FD_INDEX+1) return -1;
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
  lock_acquire(&filesys_lock);
  int length = file_length (thread_current ()->file_ptrs[fd]);
  lock_release(&filesys_lock);
  return length;
}

static int
read (int fd, void *buffer, unsigned length)
{
  struct thread *t = thread_current ();
  if (fd == 1 || fd >= t->next_open_file_index || t->file_ptrs[fd] == NULL)
    return -1;

  if (!addr_valid(buffer) || !addr_valid(buffer + length)) 
    return -1;

  if (length == 0)
    return 0;

  if (fd == 0) {
    int index = 0;
    char c;
    while (index != length) {
      c = input_getc();
      memcpy (buffer + index, &c, 1);
      index++;
    }    
    return index;
  }

  lock_acquire(&filesys_lock);
  int length = file_read (t->file_ptrs[fd], buffer, length);
  lock_release(&filesys_lock);
  return length;
}

static int
write (int fd, const void *buffer, unsigned length)
{
  struct thread *t = thread_current();
  if (fd == 0 || fd >= t->next_open_file_index || t->file_ptrs[fd] == NULL)
    return -1;   
  
  if (!addr_valid(buffer) || !addr_valid(buffer + length))
    return -1;

  if (length == 0) 
    return 0;

  if (fd == 1) {
    int num_writes = length / MAX_WRITE_SIZE + 1;
    void *src = buffer;
    while (num_writes > 0) {
      if (num_writes == 1) { 
        putbuf (src, length % MAX_WRITE_SIZE);
      } else {
        putbuf(src, MAX_WRITE_SIZE);
        src += MAX_WRITE_SIZE;
      }
    }
    return length;
  } 
  
  lock_acquire(&filesys_lock);
  int length = file_write (t->file_ptrs[fd], buffer, length);
  lock_release(&filesys_lock);
  return length;
}

static void
seek (int fd, unsigned position)
{
  if (fd == 0 || fd == 1 || fd >= t->next_open_file_index || t->file_ptrs[fd] == NULL)
    return;
  
  lock_acquire(&filesys_lock); 
  file_seek (thread_current ()->file_ptrs[fd], position);
  lock_release(&filesys_lock);
}

static unsigned
tell (int fd)
{
  if (fd == 0 || fd == 1 || fd >= t->next_open_file_index || t->file_ptrs[fd] == NULL)
     return -1; 
  
  lock_acquire(&filesys_lock);
  int position = file_tell(thread_current ()->file_ptrs[fd]);
  lock_release(&filesys_lock);
  return position;
}

static void
close (int fd)
{
  if (fd == 0 || fd == 1 || fd >= t->next_open_file_index || t->file_ptrs[fd] == NULL)
     return; 
  
  struct thread *t = thread_current ();

  lock_acquire(&filesys_lock);
  file_close (t->file_ptrs[fd]);
  lock_release(&filesys_lock);

  t->file_ptrs[fd] = NULL;
  t->next_open_file_index = fd;
}


