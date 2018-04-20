
#pragma once
#include <common/basic_defs.h>
#include <exos/hal.h>
#include <exos/interrupts.h>
#include <exos/process.h>

static inline bool in_fault_resumable_code(void)
{
   return get_curr_task()->faults_resume_mask != 0;
}

static inline bool is_fault_resumable(u32 fault_num)
{
   ASSERT(fault_num <= 31);
   return (1 << fault_num) & get_curr_task()->faults_resume_mask;
}

void handle_resumable_fault(regs *r);
int fault_resumable_call(u32 faults_mask, void *func, u32 nargs, ...);