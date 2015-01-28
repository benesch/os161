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
 * SYNCHRONIZATION PROBLEM 1: KEEBLER ELVES
 *
 * The Keebler Cookie Factory is staffed by one supervisor and many
 * elves. Each elf must complete some series of tasks before it can
 * leave for the day (implemented in the `work` function). Whenever
 * an elf completes one of its tasks, it announces what it just did
 * (implemented in the `kprintf` in the `work` function). When an elf
 * has completed all of its work the supervisor dismisses the elf by
 * saying "Thanks for your work, Elf N!" where N corresponds to the N-th elf.
 *
 * At the beginning of the day, the supervisor (a supervisor thread)
 * opens the factory and lets the elves inside (starts their threads).
 * At any given moment, there is a single supervisor and possibly
 * multiple elves working. The supervisor is not allowed to dismiss an
 * elf until that elf has finished working. Your solution CANNOT wait
 * for ALL the elves to finish before starting to dismiss them.
 */

#include <types.h>
#include <lib.h>
#include <wchan.h>
#include <thread.h>
#include <synch.h>
#include <test.h>
#include <kern/errno.h>
#include "common.h"

#define NUM_TASKS 16

static const char *tasks[NUM_TASKS] = {
	"Stirred the marshmallow mush",
	"Melted the dark chocolate",
	"Salted the caramel",
	"Fluffed the meringue",
	"Counted the butterscotch chips",
	"Chopped up the mint",
	"Chopped up the sprinkles",
	"Whipped up the cream",
	"Tasted the toffee",
	"Cooled the fudge",
	"Mixed the molasses",
	"Froze the frosting",
	"Sliced the sugar cookies",
	"Baked the apples",
	"Melted the candy coating",
	"Perfected the plum sauce",
};

/*
 * Do not modify this!
 */
static
void
work(unsigned elf_num)
{
	int r;

	r = random() % NUM_TASKS;
	while (r != 0) {
		kprintf("Elf %3u: %s\n", elf_num, tasks[r]);
		r = random() % NUM_TASKS;
		thread_yield(); // cause some interleaving!
	}
}

// One of these structs should be passed from the main driver thread
// to the supervisor thread.
struct supervisor_args {
	// Add stuff as necessary
};

// One of these structs should be passed from the supervisor thread
// to each of the elf threads.
struct elf_args {
	// Add stuff as necessary
};

static
void
elf(void *args, unsigned long junk)
{
	struct elf_args *eargs = (struct elf_args *) args;

	(void) junk; // suppress unused warnings
	(void) eargs; // suppress unused warnings
	// TODO
}

static
void
supervisor(void *args, unsigned long junk)
{
	struct supervisor_args *sargs = (struct supervisor_args *) args;

	(void) junk; // suppress unused warnings
	(void) sargs; // suppress unused warnings
	// TODO
}

int
elves(int nargs, char **args)
{
	unsigned num_elves;

	// if an argument is passed, use that as the number of elves
	num_elves = 10;
	if (nargs == 2) {
		num_elves = atoi(args[1]);
	}

	// Suppress unused warnings. Remove these when finished.
	(void) work;
	(void) supervisor;
	(void) elf;
    (void) num_elves;
	// TODO

	return 0;
}
