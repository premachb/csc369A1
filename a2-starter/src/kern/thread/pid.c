

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
 * Process ID management.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <limits.h>
#include <lib.h>
#include <array.h>
#include <clock.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <pid.h>

/*
 * Structure for holding PID and return data for a thread.
 *
 * If pi_ppid is INVALID_PID, the parent has gone away and will not be
 * waiting. If pi_ppid is INVALID_PID and pi_exited is true, the
 * structure can be freed.
 */
struct pidinfo {
	pid_t pi_pid;			// process id of this thread
	pid_t pi_ppid;			// process id of parent thread
	volatile bool pi_exited;	// true if thread has exited
	int pi_exitstatus;		// status (only valid if exited)
	struct cv *pi_cv;		// use to wait for thread exit
	bool detached;
	int joined_pid;
	pid_t kids[PROCS_MAX];
	int kids_tail;
};




/*
 * Global pid and exit data.
 *
 * The process table is an el-cheapo hash table. It's indexed by
 * (pid % PROCS_MAX), and only allows one process per slot. If a
 * new pid allocation would cause a hash collision, we just don't
 * use that pid.
 */
static struct lock *pidlock;		// lock for global exit data
static struct pidinfo *pidinfo[PROCS_MAX]; // actual pid info
static pid_t nextpid;			// next candidate pid
static int nprocs;			// number of allocated pids
static struct lock *exitlock;		// lock to increment exitcount



/*
 * pi_get: look up a pidinfo in the process table.
 */
static
struct pidinfo *
pi_get(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(pid>=0);
	KASSERT(pid != INVALID_PID);
	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	if (pi==NULL) {
		return NULL;
	}
	if (pi->pi_pid != pid) {
		return NULL;
	}
	return pi;
}


/*
 * Create a pidinfo structure for the specified pid.
 */
static
struct pidinfo *
pidinfo_create(pid_t pid, pid_t ppid)
{
	struct pidinfo *pi;
	struct pidinfo *ppi;

	if(ppid != INVALID_PID){
		ppi = pi_get(ppid);
		ppi->kids[ppi->kids_tail] = pid;
		ppi->kids_tail++;
	}

	KASSERT(pid != INVALID_PID);

	pi = kmalloc(sizeof(struct pidinfo));
	if (pi==NULL) {
		return NULL;
	}

	pi->pi_cv = cv_create("pidinfo cv");
	if (pi->pi_cv == NULL) {
		kfree(pi);
		return NULL;
	}

	pi->pi_pid = pid;
	pi->pi_ppid = ppid;
	pi->pi_exited = false;
	pi->pi_exitstatus = 0xbaad;  /* Recognizably invalid value */
	pi->joined_pid = 0;
	pi->detached = false;
	pi->kids_tail = 0;

	return pi;
}

/*
 * Clean up a pidinfo structure.
 */
static
void
pidinfo_destroy(struct pidinfo *pi)
{
	KASSERT(pi->pi_exited == true);
	KASSERT(pi->pi_ppid == INVALID_PID);
	cv_destroy(pi->pi_cv);
	kfree(pi);
}

////////////////////////////////////////////////////////////

/*
 * pid_bootstrap: initialize.
 */
void
pid_bootstrap(void)
{
	int i;

	pidlock = lock_create("pidlock");
	if (pidlock == NULL) {
		panic("Out of memory creating pid lock\n");
	}

	exitlock = lock_create("exitlock");
	if (exitlock == NULL) {
		panic("Out of memory creating exit lock\n");
	}


	/* not really necessary - should start zeroed */
	for (i=0; i<PROCS_MAX; i++) {
		pidinfo[i] = NULL;
	}

	pidinfo[BOOTUP_PID] = pidinfo_create(BOOTUP_PID, INVALID_PID);
	if (pidinfo[BOOTUP_PID]==NULL) {
		panic("Out of memory creating bootup pid data\n");
	}

	nextpid = PID_MIN;
	nprocs = 1;
}


/*
 * pi_put: insert a new pidinfo in the process table. The right slot
 * must be empty.
 */
static
void
pi_put(pid_t pid, struct pidinfo *pi)
{
	KASSERT(lock_do_i_hold(pidlock));

	KASSERT(pid != INVALID_PID);

	KASSERT(pidinfo[pid % PROCS_MAX] == NULL);
	pidinfo[pid % PROCS_MAX] = pi;
	nprocs++;
}

/*
 * pi_drop: remove a pidinfo structure from the process table and free
 * it. It should reflect a process that has already exited and been
 * waited for.
 */
static
void
pi_drop(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	KASSERT(pi != NULL);
	KASSERT(pi->pi_pid == pid);

	pidinfo_destroy(pi);
	pidinfo[pid % PROCS_MAX] = NULL;
	nprocs--;
}

////////////////////////////////////////////////////////////

/*
 * Helper function for pid_alloc.
 */
static
void
inc_nextpid(void)
{
	KASSERT(lock_do_i_hold(pidlock));

	nextpid++;
	if (nextpid > PID_MAX) {
		nextpid = PID_MIN;
	}
}

/*
 * pid_alloc: allocate a process id.
 */
int
pid_alloc(pid_t *retval)
{
	struct pidinfo *pi;
	pid_t pid;
	int count;

	KASSERT(curthread->t_pid != INVALID_PID);

	/* lock the table */
	lock_acquire(pidlock);

	if (nprocs == PROCS_MAX) {
		lock_release(pidlock);
		return EAGAIN;
	}

	/*
	 * The above test guarantees that this loop terminates, unless
	 * our nprocs count is off. Even so, assert we aren't looping
	 * forever.
	 */
	count = 0;
	while (pidinfo[nextpid % PROCS_MAX] != NULL) {

		/* avoid various boundary cases by allowing extra loops */
		KASSERT(count < PROCS_MAX*2+5);
		count++;

		inc_nextpid();
	}

	pid = nextpid;

	pi = pidinfo_create(pid, curthread->t_pid);
	if (pi==NULL) {
		lock_release(pidlock);
		return ENOMEM;
	}

	pi_put(pid, pi);

	inc_nextpid();

	lock_release(pidlock);

	*retval = pid;
	return 0;
}

/*
 * pid_unalloc - unallocate a process id (allocated with pid_alloc) that
 * hasn't run yet.
 */
void
pid_unalloc(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_exited == false);
	KASSERT(them->pi_ppid == curthread->t_pid);

	/* keep pidinfo_destroy from complaining */
	them->pi_exitstatus = 0xdead;
	them->pi_exited = true;
	them->pi_ppid = INVALID_PID;

	pi_drop(theirpid);

	lock_release(pidlock);
}

/*
 * pid_detach - disavows interest in the child thread's exit status, so 
 * it can be freed as soon as it exits. May only be called by the
 * parent thread.
 */
int
pid_detach(pid_t childpid)
{

	struct pidinfo *childpi;

	lock_acquire(pidlock);

	//ERROR: if "childpid" is INVALID_PID or BOOTUP_PID then return "EINVAL"
	if(childpid == INVALID_PID || childpid == BOOTUP_PID){
		return EINVAL;
	}

	childpi = pi_get(childpid);

	/*
	1) check to see "childpid" has exited
	*/

	//ERROR: if thread "childpid" could not be found return "ESRCH"
	if (childpi == NULL){
		lock_release(pidlock);
		return ESRCH;
	}

	//ERROR: if thread "childpid" is already in deteached state then return "EINVAL"
		
	if(childpi->detached){
		lock_release(pidlock);
		return EINVAL;
	}

	//ERROR: if the calling thread is not the parent of "childpid" then return "EINVAL"
		
	else if(childpi->pi_ppid != curthread->t_pid){
		lock_release(pidlock);
		return EINVAL;
	}

	//ERROR: if "childpid" has already been joined by atleast one other thread return "EINVAL"
	else if(childpi->joined_pid == 1){
		lock_release(pidlock);
		return EINVAL;
	}

	//check if pid has already exited, if so then frees it.
	if(childpi->pi_exited){

		pi_drop(childpi->pi_pid);
	}

	//Put the childpid into the detached state
	else{
		childpi->detached = true;
	}
	

	lock_release(pidlock);

	return 0;
}

/*
 * pid_exit 
 *  - sets the exit status of this thread (i.e. curthread). 
 *  - disowns children. 
 *  - if dodetach is true, children are also detached. 
 *  - wakes any thread waiting for the curthread to exit. 
 *  - frees the PID and exit status if the curthread has been detached. 
 *  - must be called only if the thread has had a pid assigned.
 */
void
pid_exit(int status, bool dodetach)
{
	struct pidinfo *my_pi;

	
	// Implement me. Existing code simply sets the exit status.
	lock_acquire(pidlock);

	my_pi = pi_get(curthread->t_pid);
	KASSERT(my_pi != NULL);
	my_pi->pi_exitstatus = status;
	my_pi->pi_exited = true;

	/* Disown kids */
	int i;
	struct pidinfo *current_kid;
	for(i = 0; i < my_pi->kids_tail; i++){
		current_kid = pi_get(my_pi->kids[i]);

		if(current_kid != NULL){
			current_kid->pi_ppid = INVALID_PID;
			if(dodetach){
				pid_detach(current_kid->pi_pid);
			}
		}
	}
		

	//wake people up
	cv_broadcast(my_pi->pi_cv, pidlock);

	if(my_pi->detached){
		my_pi->pi_ppid = INVALID_PID;
		pi_drop(my_pi->pi_pid);
	}

	lock_release(pidlock);
}

/*
 * pid_join - returns the exit status of the thread associated with
 * targetpid (in the status argument) as soon as it is available. 
 * If the thread has not yet exited, curthread waits unless the flag 
 * WNOHANG is sent. 
 *
 */
int
pid_join(pid_t targetpid, int *status, int flags)
{

	(void) flags;


	struct pidinfo *targetpi;


	pid_t temppid;  //temporarly hold the pid so we can destroy targetpi
	int tempstatus; ///temporaly holds pid status


	lock_acquire(pidlock);

	//ERROR: if "targetpid" is INVALID_PID or BOOTUP_PID then return "EINVAL"
	if(targetpid == INVALID_PID || targetpid == BOOTUP_PID){
		lock_release(pidlock);
		return EINVAL;
	}


	targetpi = pi_get(targetpid);

	//ERROR: if "targetpid" doesnt have a pid struct then return "ESRCH"
	if(targetpi == NULL){
		lock_release(pidlock);
		return ESRCH;
	}

	//ERROR: if "targetpid" has been detached then return "EINVAL"
	else if(targetpi->detached){
		lock_release(pidlock);
		return EINVAL;
	}

	//ERROR:if "targetpid" is pid of the thread that called PID_JOIN then return "EDEADLK". 
	//MUST ADD ERROR
	else if(targetpi->pi_pid == curthread->t_pid){
		lock_release(pidlock);
		return EDEADLK;
	}

	lock_release(pidlock);

	//check if pid has ALREADY exited
	if(targetpi->pi_exited){

		
		//grab needed information
		if(!status){
			temppid = targetpi->pi_pid;
		}else{
			tempstatus = targetpi->pi_exitstatus;
			temppid = targetpi->pi_pid;	
		}

		lock_acquire(pidlock);
		targetpi->pi_ppid = INVALID_PID;
		pi_drop(targetpi->pi_pid);
		lock_release(pidlock);

	}else{ //NOT EXITED

		//increase exit count
		lock_acquire(exitlock);
		targetpi->joined_pid += 1;
		lock_release(exitlock);


		lock_acquire(pidlock);
		//wait it exit
		cv_wait(targetpi->pi_cv, pidlock);

		lock_release(pidlock);
		//grab needed imformation
		if(!status){
			temppid = targetpi->pi_pid;
		}else{
			tempstatus = targetpi->pi_exitstatus;
			temppid = targetpi->pi_pid;	
		}


		//decrease count
		lock_acquire(exitlock);
		targetpi->joined_pid -= 1;
		
		//if last person exiting, drop information
		if(targetpi->joined_pid==0){
			lock_acquire(pidlock);
			targetpi->pi_exited = true;
			targetpi->pi_ppid = INVALID_PID;
			pi_drop(targetpi->pi_pid);
			lock_release(pidlock);
		}

		lock_release(exitlock);

	}

	*status = tempstatus;
	return temppid;

}
