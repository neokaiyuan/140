#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

/* Process identifier */
typedef int pid_t;
#define PID_ERROR ((pid_t) -1)

typedef int mapid_t;

void syscall_init (void);

#endif /* userprog/syscall.h */
