/*
 *  Four symbols uavs3d references that no single solver provides. The two
 *  solvers export different subsets - solver_minimal_1 has exit but not puts
 *  or the pthread pair, solver_1 the reverse - so all four are defined here
 *  and the module links against either.
 *
 *  The pthread ones belong to uavs3d's frame-level threading, which this
 *  filter switches off (frm_threads = 1) because side modules are
 *  single-threaded: they exist to satisfy the linker and are never called.
 */

#include <stdio.h>
#include <stdlib.h>

int puts(const char *s)
{
	if (s)
		fputs(s, stderr);
	fputs("\n", stderr);
	return 0;
}

void exit(int status)
{
	fprintf(stderr, "[AVS3Dec] uavs3d called exit(%d)\n", status);
	abort();
}

int pthread_cond_broadcast(void *cond)
{
	(void)cond;
	/* no worker threads to wake: nothing is ever waiting */
	return 0;
}

void pthread_exit(void *value)
{
	(void)value;
	/* only reachable from a worker thread, and there are none */
	abort();
}
