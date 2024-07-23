#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/fcntl.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <test.h>
#include <vfs.h>
#include <vm.h>

#include "opt-A1.h"
#include "opt-A3.h"
#include <../arch/mips/include/trapframe.h>
#include <clock.h>

#define MAX_ARGS 16
#define MAX_ARG_SIZE 128


void args_free(char **arg_list, int argc);
int argcopy_in(char **argv, char **arg_list, int argc);
char **args_alloc(char **argv, int argc);

#if OPT_A3
char **args_alloc(char ** argv, int argc) {
  char **args = kmalloc((argc + 1) * sizeof(char *));
  args[argc] = NULL;
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(argv[i]) + 1;
    args[i] = kmalloc(len);
    int err = copyin((userptr_t)argv[i], (void *)args[i], len);
    KASSERT(err == 0);
  }

  return args;
  // char **args = kmalloc((MAX_ARGS + 1) * sizeof(char *));
  // args[MAX_ARGS] = NULL;
  // for (int i = 0; i < MAX_ARGS; i++) {
  //   size_t size = strlen(argv[i]) + 1;
  //   args[i] = kmalloc(MAX_ARG_SIZE + 1);
  //   int err = copyinstr((userptr_t)argv[i], (char *)args[i], size, NULL);
  //   KASSERT(err == 0);
  // }
  // return args;
}
#endif

#if OPT_A3
void args_free(char **arg_list, int argc) {
  for (int i = 0; i < argc; i++) {
    kfree(arg_list[i]);
  }
  kfree(arg_list);
}
#endif

// #if OPT_A3
// int argcopy_in(char **argv, char **arg_list, int argc) {
//   int count = 0;
//   for (int i = 0; i < argc; i++) {
//     int size = strlen(argv[i] + 1);
//     int err = copyinstr((userptr_t)argv[i], (void *)arg_list[i], (size_t)size, NULL);
//     KASSERT(err == 0);
//     count++;
//   }

//   return count;
// }
// #endif

/* this implementation of sys__exit does not do anything with the exit code */
/* this needs to be fixed to get exit() and waitpid() working properly */
#if OPT_A3
int sys_execv(char *progname, char **argv) {
  struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

  int argc = 0;
  for (int i = 0; argv[i] != NULL; i++) {
    argc++;
  }

  char **kargv = args_alloc(argv, argc);

  // int args_copied = argcopy_in(argv, kargv, argc);
  // (void)(args_copied);
  // size_t name_size = strlen(progname + 1);
  // char *kprogname = kmalloc(name_size);
  // copyin((userptr_t)progname, (void *)kprogname, name_size);
	
  /* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	// KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	struct addrspace *as_old = curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	#if OPT_A3
	userptr_t us_argv[argc + 1];
	for (int i = argc; i >= 0; i--) {
		if (i == argc) {
			us_argv[i] = (userptr_t) NULL;
			continue;
		}
		us_argv[i] = argcopy_out(&stackptr, kargv[i]);
	}
	for (int i = argc; i >= 0; i--) {
		size_t size = sizeof(vaddr_t);
		stackptr -= size;
		int err = copyout((void *)&us_argv[i], (userptr_t)stackptr, size);
		KASSERT(err == 0);
	}

  as_destroy(as_old);
	#endif

	#if OPT_A3
  args_free(kargv, argc);
  // kfree(kprogname);
	enter_new_process(argc /*argc*/, (userptr_t)stackptr /*userspace addr of argv*/,
			  stackptr, entrypoint);
	#else
	/* Warp to user mode. */
	enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  stackptr, entrypoint);
	#endif
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
#endif

void sys__exit(int exitcode)
{

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;

  DEBUG(DB_SYSCALL, "Syscall: _exit(%d)\n", exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);
#if OPT_A1
  while (1)
  {
    if (array_num(p->p_children) == 0) {
	    break;
    }
    struct proc *temp_child;
    temp_child = (struct proc *)array_get(p->p_children, 0);
    array_remove(p->p_children, 0);
    spinlock_acquire(&temp_child->p_lock);
    if (temp_child->p_exitstatus == 1)
    {
      spinlock_release(&temp_child->p_lock);
      proc_destroy(temp_child);
    }
    else
    {
      temp_child->p_parent = NULL;
      spinlock_release(&temp_child->p_lock);
    }
  }
#endif

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

#if OPT_A1
  spinlock_acquire(&p->p_lock);
  if (p->p_parent == NULL)
  {
    spinlock_release(&p->p_lock);
    proc_destroy(p);
  }
  else
  {
    p->p_exitstatus = 1;
    p->p_exitcode = exitcode;
    spinlock_release(&p->p_lock);
  }
#else
  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
#endif

  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}

/* stub handler for getpid() system call                */
int sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
#if OPT_A1
  struct proc *p = curproc;
  *retval = p->p_pid;
  return (0);
#else
  *retval = 1;
  return (0);
#endif
}

#if OPT_A1
int sys_fork(pid_t *retval, struct trapframe *tf)
{
  struct proc *child = proc_create_runprogram("child");
  child->p_parent = curproc;
  unsigned int index = 0;
  array_add(child->p_parent->p_children, child, &index);
  as_copy(curproc_getas(), &child->p_addrspace);
  struct trapframe *trapframe_for_child = kmalloc(sizeof(struct trapframe));
  *trapframe_for_child = *tf;
  thread_fork("child_thread", child, enter_forked_process, trapframe_for_child, 0);
  *retval = child->p_pid;
  clocksleep(1);
  return 0;
}

#endif

/* stub handler for waitpid() system call  */

int sys_waitpid(pid_t pid,
                userptr_t status,
                int options,
                pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0)
  {
    return (EINVAL);
  }

#if OPT_A1
  unsigned int len = array_num(curproc->p_children);
  unsigned int index = 0;
  struct proc *temp_child;
  while (1) {
    if (index >= len) {
      return (ESRCH);
    }
    struct proc *child = (struct proc *)array_get(curproc->p_children, index);
    if (child->p_pid == pid) {
      temp_child = child;
      array_remove(curproc->p_children, index);
      break;
    }
    index++;
  }

  // we add busy polling to wait until the child has exited
  spinlock_acquire (&temp_child->p_lock);
  while (!temp_child->p_exitstatus) {
    spinlock_release(&temp_child->p_lock);
    clocksleep(1);
    spinlock_acquire(&temp_child->p_lock);
  }
  spinlock_release(&temp_child->p_lock);

  exitstatus = _MKWAIT_EXIT(temp_child->p_exitcode);
  proc_destroy(temp_child);

  result = copyout((void *)&exitstatus, status, sizeof(int));
  if (result)
  {
    return (result);
  }
  *retval = pid;
  return (0);
#else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  result = copyout((void *)&exitstatus, status, sizeof(int));
  if (result)
  {
    return (result);
  }
  *retval = pid;
  return (0);
#endif
}
