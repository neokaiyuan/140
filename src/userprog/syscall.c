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
    //Below, not thread_exit, instead call our function
    thread_exit();
  }

  bool retbool;
  pid_t retpid;
  unsigned retunsigned;
  int status, retval;
  int syscall_num = *(int *) esp;  
  switch (syscall_num) {
    case SYS_HALT: 
      halt ();
      break;
    case SYS_EXIT:
      status = *(int *) get_arg_n(1, esp);
      memcpy (&f->eax, &status, sizeof(int));
      exit (status);
      break;
    case SYS_EXEC:
      retpid = exec (*(char **) get_arg_n(1, esp));
      memcpy(&f->eax, &retpid, sizeof(pid_t));
      break;
    case SYS_WAIT:
      retval = wait (*(pid_t *) get_arg_n(1, esp));
      memcpy(&f->eax, &retval, sizeof(int));
      break;
    case SYS_CREATE:
      retbool = create (*(char **) get_arg_n(1, esp), 
             *(unsigned *) get_arg_n(2, esp));
      memcpy(&f->eax, &retbool, sizeof(bool));
      break;
    case SYS_REMOVE:
      retbool = remove (*(char **) get_arg_n(1, esp)); 
      memcpy(&f->eax, &retbool, sizeof(bool));
      break; 
    case SYS_OPEN:
      retval = open (*(char **) get_arg_n(1, esp));
      memcpy(&f->eax, &retval, sizeof(int));
      break;
    case SYS_FILESIZE:
      retval = filesize (*(int *) get_arg_n(1, esp));
      memcpy(&f->eax, &retval, sizeof(int));
      break;
    case SYS_READ:
      retval = read (*(int *) get_arg_n (1, esp), 
             *(void **) get_arg_n(2, esp), *(unsigned *) get_arg_n(3, esp));
      memcpy(&f->eax, &retval, sizeof(int));
      break;
    case SYS_WRITE:
      retval = write (*(int *) get_arg_n(1, esp), 
             *(void **) get_arg_n(2, esp), *(unsigned *) get_arg_n(3, esp));
      memcpy(&f->eax, &retval, sizeof(int));
      break;
    case SYS_SEEK:
      seek (*(int *) get_arg_n(1, esp), *(unsigned *) get_arg_n(2, esp));
      break;
    case SYS_TELL:
      retunsigned = tell (*(int *) get_arg_n(1, esp));
      memcpy(&f->eax, &retunsigned, sizeof(unsigned));
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
  shutdown_power_off();
}

static void
exit (int status)
{
  // need to implement returning status to parent
  ASSERT (status != RUNNING_EXIT_STATUS);
  struct thread *t = thread_current ();
  struct list_elem *e;
  if (t->parent != NULL) {
    for (e = list_begin (&t->parent->children_exit_info);
        e != list_end (&t->parent->children_exit_info); e = list_next (e)) {
      struct exit_info *info = list_entry (e, struct exit_info, elem);
      if (info->tid == t->tid) {
        info->exit_status = status;
        info->child = NULL;
        break;
      }
    }
  }

  printf("%s: exit(%d)\n", t->name, status);
  thread_exit ();
}

static pid_t
exec (const char *file)
{
  pid_t pid = (pid_t) process_execute (file);
  if (pid == TID_ERROR) return -1;
  sema_down(&thread_current()->child_sema); // look at start_process
  if (thread_current()->child_exec_status == false) return -1;
  return pid; 
}

static int
wait (pid_t pid)
{
  return process_wait (pid);
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


