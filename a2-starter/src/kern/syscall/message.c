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
#include <pid.h>


int sendmessage(pid_t pid, void * buf, size_t len, int flags){
	(void) pid;
	(void) buf;
	(void) len;
	(void) flags;

	return 0;
}

int recvmessage(void * buf, size_t maxsize, pid_t *sender, int flags){
	(void) buf;
	(void) maxsize;
	(void) sender;
	(void) flags;

	return 0;
}

