/* spurs_task.h -- SPURS leaf-task lifecycle helpers. See spurs_task.c. */
#ifndef SPURS_TASK_H
#define SPURS_TASK_H

#include "spu_context.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On a SPURS leaf task's `stop 0` (CELL_SPURS_TASK_SYSCALL_EXIT), write its exit
 * code into the registered exit-code container (guest EA). Returns 1 if the task
 * EXITed (and the code was written), 0 otherwise. */
int spu_spurs_task_write_exit_code(spu_context* ctx, uint32_t exitcode_ea);

#ifdef __cplusplus
}
#endif

#endif /* SPURS_TASK_H */
