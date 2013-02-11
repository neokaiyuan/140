#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include "lib/string.h"


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

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  void *esp = f->esp;
  if (!addr_valid (esp)) {
    //Stop process
    //Free Assocated memory
    thread_exit();
  }  
  int syscall_num = *(int *) esp;
  
  switch (syscall_num) {
    case SYS_HALT: 
      halt ();
      break;
    case SYS_EXIT:
      exit (*(int *) get_arg_n(1, esp));
      break;
    case SYS_EXEC:
      pid_t pid = exec (*(char **) get_arg_n(1, esp));
      memcpy(&f->eax, &pid, sizeof(pid_t));
      break;
    case SYS_WAIT:
      int to_return = wait (*(pid_t *) get_arg_n(1, esp));
      memcpy(&f->eax, &to_return, sizeof(int));
      break;
    case SYS_CREATE:
      bool to_return = create (*(char **) get_arg_n(1, esp), 
             *(unsigned *) get_arg_n(2, esp));
      memcpy(&f->eax, &to_return, sizeof(bool));
      break;
    case SYS_REMOVE:
      bool to_return = remove (*(char **) get_arg_n(1, esp)); 
      memcpy(&f->eax, &to_return, sizeof(bool));
      break; 
    case SYS_OPEN:
      int to_return = open (*(char **) get_arg_n(1, esp));
      memcpy(&f->eax, &to_return, sizeof(int));
      break;
    case SYS_FILESIZE:
      int to_return = filesize (*(int *) get_arg_n(1, esp));
      memcpy(&f->eax, &to_return, sizeof(int));
      break;
    case SYS_READ:
      int to_return = read (*(int *) get_arg_n (1, esp), 
             *(void **) get_arg_n(2, esp), *(unsigned *) get_arg_n(3, esp));
      memcpy(&f->eax, &to_return, sizeof(int));
      break;
    case SYS_WRITE:
      int to_return = write (*(int *) get_arg_n(1, esp), 
             *(void **) get_arg_n(2, esp), *(unsigned *) get_arg_n(3, esp));
      memcpy(&f->eax, &to_return, sizeof(int));
      break;
    case SYS_SEEK:
      seek (*(int *) get_arg_n(1, esp), *(unsigned *) get_arg_n(2, esp));
      break;
    case SYS_TELL:
      unsigned to_return = tell (*(int *) get_arg_n(1, esp));
      memcpy(&f->eax, &to_return, sizeof(unsigned));
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
  if (ptr < 0 || ptr >= PHYS_BASE) return false;
  if (lookup_page (thread_current()->pagedir, ptr, false) == NULL) 
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
// NO RETURN
}

static void
exit (int status)
{
// NO RETURN
}

static pid_t
exec (const char *file)
{

}

static int
wait (pid_t)
{

}

static bool
create (const char *file, unsigned intial_size)
{

}

static bool
remove (const char *file)
{

}

static int
open (const char *file)
{

}

static int
filesize (int fd)
{

}

static int
read (int fd, void *buffer, unsigned length)
{

}

static int
write (int fd, const void *buffer, unsigned length)
{

}

static void
seek (int fd, unsigned position)
{

}

static unsigned
tell (int fd)
{

}

static void
close (int fd)
{

}


