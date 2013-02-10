#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
//1) Print out the system call number
//2) Have a switch or 'if' statements to call the correct function
//Note. SysCallNumbers #defined in lib/syscall-nr.h
  printf ("system call!\n");
  thread_exit ();
}
