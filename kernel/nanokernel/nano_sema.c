/* nanokernel semaphore object */

/*
 * Copyright (c) 2010-2015 Wind River Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1) Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2) Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3) Neither the name of Wind River Systems nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
DESCRIPTION
This module provides the VxMicro nanokernel (aka system-level) 'sema'
implementation.  This module provides the backing implementation for the
following APIs:

   nano_sem_init
   nano_fiber_sem_give, nano_task_sem_give, nano_isr_sem_give
   nano_fiber_sem_take, nano_task_sem_take, nano_isr_sem_take
   nano_fiber_sem_take_wait, nano_task_sem_take_wait

The semaphores are of the 'counting' type, i.e. each 'give' operation will
increment the internal count by 1, if no context is pending on it. The 'init'
call initializes the count to 0. Following multiple 'give' operations, the
same number of 'take' operations can be performed without the calling context
having to pend on the semaphore.

INTERNAL
In some cases the compiler "alias" attribute is used to map two or more
APIs to the same function, since they have identical implementations.
*/

#include <nanok.h>
#include <toolchain.h>
#include <sections.h>

/*******************************************************************************
*
* nano_sem_init - initialize a nanokernel semaphore object
*
* This function initializes a nanokernel semaphore object structure. After
* initialization, the semaphore count will be 0.
*
* It may be called from either a fiber or task context.
*
* RETURNS: N/A
*
* INTERNAL
* Although the existing implementation will support invocation from an ISR
* context, for future flexibility, this API will be restricted from ISR
* level invocation.
*/

void nano_sem_init(
	struct nano_sem *sem /* semaphore object to initialize */
	)
{
	sem->nsig = 0;
	sem->proc = (tCCS *)0;
}

FUNC_ALIAS(_sem_give, nano_isr_sem_give, void);
FUNC_ALIAS(_sem_give, nano_fiber_sem_give, void);

/*******************************************************************************
*
* _sem_give - give a nanokernel semaphore
*
* This routine performs a "give" operation on a nanokernel sempahore object;
* it may be call from either a fiber or an ISR context.  A fiber pending on
* the semaphore object will be made ready, but will NOT be scheduled to
* execute.
*
* RETURNS: N/A
*
* INTERNAL
* This function is capable of supporting invocations from both a fiber and an
* ISR context.  However, the nano_isr_sem_give and nano_fiber_sem_give aliases
* are created to support any required implementation differences in the future
* without introducing a source code migration issue.
*/

void _sem_give(
	struct nano_sem *sem /* semaphore on which to signal */
	)
{
	tCCS *ccs;
	unsigned int imask;

	imask = irq_lock_inline();
	ccs = sem->proc;
	if (ccs != (tCCS *)NULL) {
		sem->proc = 0;
		_insert_ccs((tCCS **)&_NanoKernel.fiber, ccs);
	} else {
		sem->nsig++;
	}

	irq_unlock_inline(imask);
}

/*******************************************************************************
*
* nano_task_sem_give - give a nanokernel semaphore
*
* This routine performs a "give" operation on a nanokernel sempahore object;
* it can only be called from a task context.  A fiber pending on the
* semaphore object will be made ready, and will be scheduled to execute.
*
* RETURNS: N/A
*/

void nano_task_sem_give(
	struct nano_sem *sem /* semaphore on which to signal */
	)
{
	tCCS *ccs;
	unsigned int imask;

	imask = irq_lock_inline();
	ccs = sem->proc;
	if (ccs != (tCCS *)NULL) {
		sem->proc = 0;
		_insert_ccs((tCCS **)&_NanoKernel.fiber, ccs);

		/* swap into the newly ready fiber */

		_Swap(imask);
		return;
	} else {
		sem->nsig++;
	}

	irq_unlock_inline(imask);
}

/*******************************************************************************
*
* nano_sem_give - give a nanokernel semaphore
*
* This is a convenience wrapper for the context-specific APIs. This is
* helpful whenever the exact scheduling context is not known, but should
* be avoided when the context is known up-front (to avoid unnecessary
* overhead).
*/

void nano_sem_give(struct nano_sem *sem)
{
	static void (*func[3])(struct nano_sem *sem) = {
		nano_isr_sem_give, nano_fiber_sem_give, nano_task_sem_give
	};
	func[context_type_get()](sem);
}

FUNC_ALIAS(_sem_take, nano_isr_sem_take, int);
FUNC_ALIAS(_sem_take, nano_fiber_sem_take, int);
FUNC_ALIAS(_sem_take, nano_task_sem_take, int);

/*******************************************************************************
*
* _sem_take - take a nanokernel semaphore, fail if unavailable
*
* Attempt to take a nanokernel sempahore; it may be called from a fiber, task,
* or ISR context.
*
* If the semaphore is not available, this function returns immediately, i.e.
* a wait (pend) operation will NOT be performed.
*
* RETURNS: 1 if semaphore is available, 0 otherwise
*/

int _sem_take(
	struct nano_sem *sem /* semaphore on which to test */
	)
{
	unsigned int imask;
	int avail;

	imask = irq_lock_inline();
	avail = (sem->nsig > 0);
	sem->nsig -= avail;
	irq_unlock_inline(imask);

	return avail;
}

/*******************************************************************************
*
* nano_fiber_sem_take_wait - test a nanokernel semaphore, wait if unavailable
*
* Take a nanokernel sempahore; it can only be called from a fiber context.
*
* If the nanokernel semaphore is not available, i.e. the event counter
* is 0, the calling fiber context will wait (pend) until the semaphore is
* given (via nano_fiber_sem_give/nano_task_sem_give/nano_isr_sem_give).
*
* RETURNS: N/A
*
* INTERNAL
* There exists a separate nano_task_sem_take_wait() implementation since a task
* context cannot pend on a nanokernel object.  Instead, tasks will poll
* the sempahore object.
*/

void nano_fiber_sem_take_wait(
	struct nano_sem *sem /* semaphore on which to wait */
	)
{
	unsigned int imask;

	imask = irq_lock_inline();
	if (sem->nsig == 0) {
		sem->proc = _NanoKernel.current;
		_Swap(imask);
	} else {
		sem->nsig--;
		irq_unlock_inline(imask);
	}
}

/*******************************************************************************
*
* nano_task_sem_take_wait - take a nanokernel semaphore, poll if unavailable
*
* Take a nanokernel sempahore; it can only be called from a task context.
*
* If the nanokernel semaphore is not available, i.e. the event counter
* is 0, the calling task will poll until the semaphore is given
* (via nano_fiber_sem_give/nano_task_sem_give/nano_isr_sem_give).
*
* RETURNS: N/A
*/

void nano_task_sem_take_wait(
	struct nano_sem *sem /* semaphore on which to wait */
	)
{
	unsigned int imask;

	/* spin until the sempahore is signaled */

	while (1) {
		imask = irq_lock_inline();

		/*
		 * Predict that the branch will be taken to break out of the loop.
		 * There is little cost to a misprediction since that leads to idle.
		 */

		if (likely(sem->nsig > 0))
			break;

		/* see explanation in nano_stack.c:nano_task_stack_pop_wait() */

		nano_cpu_atomic_idle(imask);
	}

	sem->nsig--;
	irq_unlock_inline(imask);
}

/*******************************************************************************
*
* nano_sem_take_wait - take a nanokernel semaphore, poll/pend if not available
*
* This is a convenience wrapper for the context-specific APIs. This is
* helpful whenever the exact scheduling context is not known, but should
* be avoided when the context is known up-front (to avoid unnecessary
* overhead).
*
* It's only valid to call this API from a fiber or a task.
*/
void nano_sem_take_wait(struct nano_sem *sem)
{
	static void (*func[3])(struct nano_sem *sem) = {
		NULL, nano_fiber_sem_take_wait, nano_task_sem_take_wait
	};
	func[context_type_get()](sem);
}
