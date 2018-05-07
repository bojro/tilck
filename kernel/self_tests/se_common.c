
#include <common/basic_defs.h>
#include <common/string_util.h>
#include <common/utils.h>

#include <exos/irq.h>
#include <exos/kmalloc.h>
#include <exos/paging.h>
#include <exos/debug_utils.h>
#include <exos/process.h>

#include <exos/hal.h>
#include <exos/tasklet.h>
#include <exos/sync.h>
#include <exos/fault_resumable.h>

void simple_test_kthread(void *arg)
{
   int i;
   uptr saved_esp;
   uptr esp;

   printk("[kthread] This is a kernel thread, arg = %p\n", arg);

   saved_esp = get_curr_stack_ptr();

   for (i = 0; i < 1024*(int)MB; i++) {

      /*
       * This VERY IMPORTANT check ensures us that in NO WAY functions like
       * save_current_task_state() and kernel_context_switch() changed value
       * of the stack pointer.
       */
      esp = get_curr_stack_ptr();
      VERIFY(esp == saved_esp);

      if (!(i % (256*MB))) {
         printk("[kthread] i = %i\n", i/MB);
      }
   }

   debug_qemu_turn_off_machine();
}

void selftest_kthread(void)
{
   kthread_create(simple_test_kthread, (void *)0xAA0011FF);
}

void sleeping_kthread(void *arg)
{
   u64 wait_ticks = (uptr) arg;
   u64 before = get_ticks();

   kernel_sleep(wait_ticks);

   u64 after = get_ticks();
   u64 elapsed = after - before;

   printk("[sleeping_kthread] elapsed ticks: %llu (expected: %llu)\n",
          elapsed, wait_ticks);

   VERIFY((elapsed - wait_ticks) <= 2);

   debug_qemu_turn_off_machine();
}

void selftest_kernel_sleep(void)
{
   kthread_create(sleeping_kthread, (void *)TIMER_HZ);
}


void kthread_panic(void)
{
   for (int i = 0; i < 1000*1000*1000; i++) {
      asmVolatile("nop");
   }

   panic("test panic");
}

void selftest_panic(void)
{
   kthread_create(&kthread_panic, NULL);
}