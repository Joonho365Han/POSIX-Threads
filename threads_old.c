/*
*	Copyright Joonho Han
*	joonho18@bu.edu
*/
// NOTE: This assignment assumes only one thread can run at a time = NO MULTICORE
// NOTE: This assignment is architecture specific to: 32bit Linux bandit 3.16.0-4-amd64 #1 SMP Debian 3.16.36-1+deb8u1 (2016-09-03) x86_64 GNU/Linux

#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#define THREAD_STACK_SIZE 32767

#define THREAD_RUNNING 	3
#define THREAD_READY 	2
#define THREAD_BLOCKED	1
#define THREAD_EXITED 	0

#define JB_BX 0 
#define JB_SI 1 
#define JB_DI 2 
#define JB_BP 3 
#define JB_SP 4 
#define JB_PC 5

/* START Helper functions */
static int __ptr_mangle(int p)
{
	unsigned int ret;
	asm(" movl %1, %%eax;\n"
       " xorl %%gs:0x18, %%eax;"
       " roll $0x9, %%eax;"
       " movl %%eax, %0;"
	   : "=r"(ret)
	   : "r"(p)
	   : "%eax"
    );
	return ret; 
}

void __pthread_exit_wrapper()
{
	unsigned int res;
	asm("movl %%eax, %0\n":"=r"(res));
	pthread_exit((void *) res);
}

void lock()
{
	sigset_t __sig_scheduler;
	sigemptyset(&__sig_scheduler);
	sigaddset(&__sig_scheduler, SIGALRM);

	sigprocmask(SIG_BLOCK,&__sig_scheduler,NULL);
}

void unlock()
{
	sigset_t __sig_scheduler;
	sigemptyset(&__sig_scheduler);
	sigaddset(&__sig_scheduler, SIGALRM);

	sigprocmask(SIG_UNBLOCK,&__sig_scheduler,NULL);
}
/* END Helper functions */



/* START pthread implementation */
typedef struct __listener 
{
	int id;
	struct __listener *next;
} __listener;

typedef struct
{
	pthread_t  id;
	int     state;
	void *t_stack;
	void *exitval;
	__listener *listener;
	__listener *first_in_line;
	struct __jmp_buf_tag jmp_buf;
} TCB;

TCB 		*__tcb_list[88000]; //  Array of TCB pointers. There's a limit to how many threads can be created
int 		__tcb_extra = 0; //  Number of active NON-MAIN TCBs
pthread_t 	__curr_t_id = 0; //  The running thread's ID
pthread_t	__new_tcbid = 0; //  New thread ID to be assigned

void __usermode_scheduler(int alarm) //  ::: This function cannot have any local variables :::
{
	//  Change states of threads
	lock();
	if (__tcb_list[__curr_t_id]->state == THREAD_RUNNING)
		__tcb_list[__curr_t_id]->state = THREAD_READY;
	else if (__tcb_list[__curr_t_id]->state == THREAD_EXITED)
		while (__tcb_list[__curr_t_id]->listener!=NULL)
		{
			__tcb_list[__curr_t_id]->first_in_line = __tcb_list[__curr_t_id]->listener;
			__tcb_list[__tcb_list[__curr_t_id]->first_in_line->id]->state = THREAD_READY;
			__tcb_list[__curr_t_id]->listener = __tcb_list[__curr_t_id]->first_in_line->next;
			free(__tcb_list[__curr_t_id]->first_in_line);
		}

	//  Save state of thread OR return to thread execution.
	if (setjmp(&__tcb_list[__curr_t_id]->jmp_buf)) 
		return;

	//  Find ready thread other than current one
	do {
		__curr_t_id = (__curr_t_id+1) % __new_tcbid;
	}
	while (__tcb_list[__curr_t_id]->state != THREAD_READY);
	__tcb_list[__curr_t_id]->state = THREAD_RUNNING;

	//  Reactive scheduler only when there are extra threads.
	if (__tcb_extra > 0) 
		unlock();

	//  Execute new thread
	longjmp(&__tcb_list[__curr_t_id]->jmp_buf, 1);
}

int pthread_create(
	pthread_t *thread,
	const pthread_attr_t *attr,
	void *(*start_routine) (void *),
	void *arg)
{
	if (__new_tcbid == 0) // No threads in TCB list (not even the main thread)
	{	
		//  Add main thread's TCB before making new thread.
		__tcb_list[0] = (TCB*) malloc(sizeof(TCB));
		__tcb_list[0]->id = __new_tcbid++;
		__tcb_list[0]->state = THREAD_RUNNING;

		//  Activate scheduler
		struct sigaction handler;
  		sigemptyset(&handler.sa_mask);
		handler.sa_handler = __usermode_scheduler;
		handler.sa_flags = SA_NODEFER;
		sigaction(SIGALRM, &handler, NULL);
		ualarm(50000, 50000);
	}

	//  Initialize TCB for new thread
	lock();
	__tcb_list[__new_tcbid] = (TCB*) malloc(sizeof(TCB));
	TCB *new_tcb = __tcb_list[__new_tcbid];
	new_tcb->id = __new_tcbid++;
	new_tcb->state = THREAD_READY;
	new_tcb->t_stack = malloc(THREAD_STACK_SIZE);
	new_tcb->exitval = NULL;
	new_tcb->listener = NULL;
	new_tcb->jmp_buf.__jmpbuf[JB_BP] = __ptr_mangle((int) new_tcb->t_stack + THREAD_STACK_SIZE);
	int *esp = (int*) (new_tcb->t_stack + THREAD_STACK_SIZE);
	*(--esp) = (int) arg;
	*(--esp) = (int) __pthread_exit_wrapper;
	new_tcb->jmp_buf.__jmpbuf[JB_SP] = __ptr_mangle((int) esp);
	new_tcb->jmp_buf.__jmpbuf[JB_PC] = __ptr_mangle((int) start_routine);
	++__tcb_extra;
	unlock();

	if (thread!=NULL) *thread = new_tcb->id;

	return 0;
}

int pthread_join(pthread_t thread, void **value_ptr)
{
	//  Thread does not exist OR the joining thread is its own
	if (thread == __new_tcbid || thread == __curr_t_id) 
		return -1; 

	lock();
	if (__tcb_list[thread]->state != THREAD_EXITED)
	{
		__tcb_list[__curr_t_id]->state = THREAD_BLOCKED;
		__listener *new_head = (__listener*) malloc(sizeof(__listener));
		new_head->id = __curr_t_id;
		new_head->next = __tcb_list[thread]->listener;
		__tcb_list[thread]->listener = new_head;
		__usermode_scheduler(0);
	}
	else
		unlock();

	if (value_ptr != NULL)
		*value_ptr = __tcb_list[thread]->exitval;

	return 0;
}

void pthread_exit(void *value_ptr) //  This function cannot have any local variables as there will be no stack 
{
	if (__curr_t_id==0) exit(0);
	
	lock();
	free(__tcb_list[__curr_t_id]->t_stack);
	__tcb_list[__curr_t_id]->state = THREAD_EXITED;
	__tcb_list[__curr_t_id]->exitval = value_ptr;
	--__tcb_extra;

	//  Execute, but not call, the scheduler.
	unlock();
	setjmp(&__tcb_list[__curr_t_id]->jmp_buf);
	__tcb_list[__curr_t_id]->jmp_buf.__jmpbuf[JB_PC] = __ptr_mangle((int) __usermode_scheduler);
	longjmp(&__tcb_list[__curr_t_id]->jmp_buf, 0);
}

pthread_t pthread_self()
{
	return __curr_t_id;
}
/* END pthread implementation */



/* START semaphore implementation */
typedef struct
{
	unsigned int value;
	__listener *listener;
} __real_sem;

typedef struct __sem_addr 
{
	sem_t* address;
	struct __sem_addr* next;
} __sem_addr;

__sem_addr* __semaphore_list_ = NULL;

int sem_init(sem_t *sem, int pshared, unsigned int value) 
{
	lock();
	__sem_addr* *semaphore = &__semaphore_list_;
	while (*semaphore != NULL)
		if ((*semaphore)->address == sem)
		{
			unlock();
			return -1;
		}
		else
			semaphore = &(*semaphore)->next;

	__real_sem *new_sem = malloc(sizeof(__real_sem));
	__sem_addr *new_add = malloc(sizeof(__sem_addr));
	new_add->address = sem;
	new_add->next = NULL;
	*semaphore = new_add;

	new_sem->value = value;
	new_sem->listener = NULL;
	sem->__align = (long int) new_sem;

	unlock();
	return 0;
}

int sem_wait(sem_t *sem)
{
	lock();
	__real_sem *r_sem = (__real_sem*)sem->__align;
	__sem_addr* *semaphore = &__semaphore_list_;

	while (*semaphore != NULL)
		if ((*semaphore)->address == sem)
			break;
		else
			semaphore = &(*semaphore)->next;

	if (*semaphore == NULL)
	{
		unlock();
		return -1;
	}
	
	if (r_sem->value == 0)
	{
		__tcb_list[__curr_t_id]->state = THREAD_BLOCKED;
		__listener *new_head = (__listener*) malloc(sizeof(__listener));
		new_head->id = __curr_t_id;
		new_head->next = r_sem->listener;
		r_sem->listener = new_head;
		__usermode_scheduler(0);
	}
	else
	{
		r_sem->value--;
		unlock();
	}
	return 0;
}

int sem_post(sem_t *sem)
{
	lock();
	__real_sem *r_sem = (__real_sem*)sem->__align;
	__sem_addr* *semaphore = &__semaphore_list_;

	while (*semaphore != NULL)
		if ((*semaphore)->address == sem)
			break;
		else
			semaphore = &(*semaphore)->next;

	if (*semaphore == NULL)
	{
		unlock();
		return -1;
	}

	if (r_sem->listener != NULL)
	{
		__listener *listener = r_sem->listener;
		__tcb_list[listener->id]->state = THREAD_READY;
		r_sem->listener = listener->next;
		free(listener);
	}
	else
		r_sem->value++;
	
	unlock();
	return 0;
}

int sem_destroy(sem_t *sem)
{
	lock();
	__sem_addr* *semaphore = &__semaphore_list_;

	while (*semaphore != NULL)
		if ((*semaphore)->address == sem)
			break;
		else
			semaphore = &(*semaphore)->next;

	__real_sem *r_sem = (__real_sem*)sem->__align;
	if (*semaphore == NULL || r_sem->listener != NULL)
	{
		unlock();
		return -1;
	}

	__sem_addr* to_delete = *semaphore;
	(*semaphore) = to_delete->next;
	free(r_sem);
	free(to_delete);
	
	unlock();
	return 0;
}
/* END semaphore implementation */