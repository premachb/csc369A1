/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than this function does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <copyinout.h>

#define MAX_LEN 255

/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int
runprogram(char *progname, char **argv, unsigned long nargs)
{
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}


	/* We should be a new thread. */
	KASSERT(curthread->t_addrspace == NULL);

	/* Create a new address space. */
	curthread->t_addrspace = as_create();
	if (curthread->t_addrspace==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Activate it. */
	as_activate(curthread->t_addrspace);

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* thread_exit destroys curthread->t_addrspace */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */  
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(curthread->t_addrspace, &stackptr);

	if (result) {
		/* thread_exit destroys curthread->t_addrspace */
		return result;
	}

	size_t i;
	size_t padding;
	size_t current_offset = 0;
	size_t actual;
	size_t iterator;
	int code;
	vaddr_t *arg_start = kmalloc((nargs+1) * sizeof(userptr_t));  /* nargs * 4 bytes for each argument */

	/* Copy the strings themselves on to the stack. */
	for(i = 1; i <= nargs; i++){
		iterator = nargs - i;
		kprintf("Argv at %d is %s\n", iterator, argv[iterator]);

		current_offset+= strlen(argv[iterator]) + 1;
		*(arg_start + iterator) = (vaddr_t)(stackptr - current_offset);

		if(argv[iterator] != NULL){
			code = copyoutstr(argv[iterator], (userptr_t)*(arg_start + iterator), strlen(argv[iterator]) + 1, &actual);
		}
		
		kprintf("Codeoutstr is %d\n", code);
		kprintf("Current_Offset is %d\n", current_offset);
	}

	// Set NULL pointer for argv[argc]
	*(arg_start + nargs) = 0;

	/* Copy the array of pointer on to the stack */
	size_t accumulator = 0;
 	padding = (stackptr - current_offset) % 4;

 	
 	for(i = 0; i <= nargs; i++){
 		iterator = nargs - i;
 		accumulator += sizeof(userptr_t);
		code = copyout(&arg_start[iterator], (userptr_t)(stackptr - current_offset - padding - accumulator), sizeof(userptr_t)); 
		kprintf("Codeout is %d\n", code);
	}
	

	/* Warp to user mode. */
	enter_new_process(nargs, (userptr_t)(stackptr - current_offset - padding - accumulator), 
			  (stackptr - current_offset - padding), entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

