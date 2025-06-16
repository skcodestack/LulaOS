// #include <arch/x86/semaphore.h>
  

// void __up(struct semaphore *sem)
// {
// 	wake_up(&sem->wait);
// }

// static spinlock_t semaphore_lock = SPIN_LOCK_UNLOCKED;

 
// void __down(struct semaphore * sem)
// {
// 	struct task_struct *tsk = current; 
// 	DECLARE_WAITQUEUE(wait, tsk); 
// 	tsk->state = TASK_UNINTERRUPTIBLE; 
// 	add_wait_queue_exclusive(&sem->wait, &wait);

 
// 	spin_lock_irq(&semaphore_lock);
// 	sem->sleepers++; 
	 
// 	for (;;) {
// 		int sleepers = sem->sleepers;

		 
// 		if (!atomic_add_negative(sleepers - 1, &sem->count)) {
// 			sem->sleepers = 0;
// 			break;
// 		}
// 		sem->sleepers = 1;	/* us - see -1 above */
// 		spin_unlock_irq(&semaphore_lock); 
// 		schedule(); 
// 		tsk->state = TASK_UNINTERRUPTIBLE;
// 		spin_lock_irq(&semaphore_lock);
// 	}
// 	spin_unlock_irq(&semaphore_lock);
	 
// 	remove_wait_queue(&sem->wait, &wait);
 
// 	tsk->state = TASK_RUNNING;
	 
// 	wake_up(&sem->wait);
// }

// int __down_interruptible(struct semaphore * sem)
// {
// 	int retval = 0;
// 	struct task_struct *tsk = current;
// 	DECLARE_WAITQUEUE(wait, tsk);
// 	tsk->state = TASK_INTERRUPTIBLE;
// 	add_wait_queue_exclusive(&sem->wait, &wait);

// 	spin_lock_irq(&semaphore_lock);
// 	sem->sleepers ++;
// 	for (;;) {
// 		int sleepers = sem->sleepers;

		 
// 		if (signal_pending(current)) {
// 			retval = -EINTR;
// 			sem->sleepers = 0;
// 			atomic_add(sleepers, &sem->count);
// 			break;
// 		}

		 
// 		if (!atomic_add_negative(sleepers - 1, &sem->count)) {
// 			sem->sleepers = 0;
// 			break;
// 		}
// 		sem->sleepers = 1;	/* us - see -1 above */
// 		spin_unlock_irq(&semaphore_lock);

// 		schedule();
// 		tsk->state = TASK_INTERRUPTIBLE;
// 		spin_lock_irq(&semaphore_lock);
// 	}
// 	spin_unlock_irq(&semaphore_lock);
// 	tsk->state = TASK_RUNNING;
// 	remove_wait_queue(&sem->wait, &wait);
// 	wake_up(&sem->wait);
// 	return retval;
// }
 
// int __down_trylock(struct semaphore * sem)
// {
// 	int sleepers;
// 	unsigned long flags;

// 	spin_lock_irqsave(&semaphore_lock, flags);
// 	sleepers = sem->sleepers + 1;
// 	sem->sleepers = 0;

	 
// 	if (!atomic_add_negative(sleepers, &sem->count))
// 		wake_up(&sem->wait);

// 	spin_unlock_irqrestore(&semaphore_lock, flags);
// 	return 1;
// }

 
// asm(
// ".text\n"
// ".align 4\n"
// ".globl __down_failed\n"
// "__down_failed:\n\t"
// 	"pushl %eax\n\t"
// 	"pushl %edx\n\t"
// 	"pushl %ecx\n\t"
// 	"call __down\n\t"
// 	"popl %ecx\n\t"
// 	"popl %edx\n\t"
// 	"popl %eax\n\t"
// 	"ret"
// );

// asm(
// ".text\n"
// ".align 4\n"
// ".globl __down_failed_interruptible\n"
// "__down_failed_interruptible:\n\t"
// 	"pushl %edx\n\t"
// 	"pushl %ecx\n\t"
// 	"call __down_interruptible\n\t"
// 	"popl %ecx\n\t"
// 	"popl %edx\n\t"
// 	"ret"
// );

// asm(
// ".text\n"
// ".align 4\n"
// ".globl __down_failed_trylock\n"
// "__down_failed_trylock:\n\t"
// 	"pushl %edx\n\t"
// 	"pushl %ecx\n\t"
// 	"call __down_trylock\n\t"
// 	"popl %ecx\n\t"
// 	"popl %edx\n\t"
// 	"ret"
// );

// asm(
// ".text\n"
// ".align 4\n"
// ".globl __up_wakeup\n"
// "__up_wakeup:\n\t"
// 	"pushl %eax\n\t"
// 	"pushl %edx\n\t"
// 	"pushl %ecx\n\t"
// 	"call __up\n\t"
// 	"popl %ecx\n\t"
// 	"popl %edx\n\t"
// 	"popl %eax\n\t"
// 	"ret"
// );
 
// asm(
// "  
// .align	4
// .globl	__write_lock_failed
// __write_lock_failed:
// 	" LOCK "addl	$" RW_LOCK_BIAS_STR ",(%eax)		 
// 1:	rep; nop
// 	cmpl	$" RW_LOCK_BIAS_STR ",(%eax)				  
// 	jne	1b												 

// 	" LOCK "subl	$" RW_LOCK_BIAS_STR ",(%eax)		 
// 	jnz	__write_lock_failed								 
// 	ret													 
 
// .align	4
// .globl	__read_lock_failed
// __read_lock_failed:
// 	lock ; incl	(%eax)			 
// 1:	rep; nop
// 	cmpl	$1,(%eax)			 
// 	js	1b						 

// 	lock ; decl	(%eax)			 
// 	js	__read_lock_failed		 
// 	ret							 
// "
// ); 
