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

int waitpid(pid_t target_pid, userptr_t status, int flags){
	int value;
	int status2;
	int result;

	if(pid_is_child(target_pid)){
		value = pid_join(target_pid, &status2, flags);

		if(value != 0){
			result = copyout(&status2, status, sizeof(int));
		}
		else{
			return 0;
		}
	}
	else{
		return EINVAL;
	}

	return result;
}
