/*
 * Copyright (c) 2014
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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <wchan.h>
#include <synch.h>
#include <proc.h>
#include <current.h>
#include <buf.h>
#include <sfs.h>
#include "sfsprivate.h"

/*
 * Physical journal container.
 *
 * This file manages access to the on-disk journal.
 *
 * The interface to this module is documented in design/jphys.txt.
 */

////////////////////////////////////////////////////////////
// types

/*
 * Journal position, used during recovery
 */
struct sfs_jposition {
	uint32_t jp_jblock;	/* block index into journal */
	uint32_t jp_blockoffset;/* position in block */
};

/*
 * Physical journal (container-level) state
 *
 * jp_firstlsns is indexed by journal block number (relative to the
 * journal start block) and contains the first lsn in that journal
 * block, or 0 if the journal block in question isn't in memory.
 *
 * XXX find a new name for jp_nextlsn? it is now confusing.
 *
 * Note: jp_headjblock is the in-memory head. The on-disk head is
 * implicit in jp_writestate. (FUTURE: instead of jp_writestate, which
 * is an array of flags, move to a simpler representation that just
 * has the on-disk head.)
 *
 * The in-memory tail is jp_oldestjblock.
 *
 * The on-disk tail is not actually tracked; it's just written out
 * when we trim the log. XXX: we should track it so we can check for
 * head/tail collisions.
 */
struct sfs_jphys {
	bool jp_physrecovered;		/* container-level recovery done */
	bool jp_recoverymode;		/* recovery mode enabled */
	bool jp_operatingmode;		/* operating mode enabled */

	struct lock *jp_lock;		/* lock for the physical journal */

	struct buf *jp_headbuf;		/* buffer for journal head */
	struct buf *jp_nextbuf;		/* buffer for next journal head */
	struct thread *jp_gettingnext;	/* who's going to fetch jp_nextbuf */
	struct cv *jp_nextcv;		/* to wait for jp_nextbuf */

	uint32_t jp_headjblock;		/* journal block number of head */
	unsigned jp_headbyte;		/* byte offset for journal head */
	sfs_lsn_t jp_headfirstlsn;	/* oldest lsn in headbuf */

	sfs_lsn_t jp_nextlsn;		/* next LSN to use */

	uint32_t jp_jblockcount;	/* counter of jblocks used */

	struct spinlock jp_lsnmaplock;	/* lock for the following */
	sfs_lsn_t *jp_firstlsns;		/* first lsn in each journal block */
	uint32_t jp_oldestjblock;	/* oldest journal block in memory */

	/* These are only valid during recovery and not afterwards updated. */
	struct sfs_jposition jp_recov_tailpos;
	struct sfs_jposition jp_recov_headpos;
};

/*
 * Constants for jp_writestate
 */
#define SFS_WS_CLEAN    0
#define SFS_WS_DIRTY    1
#define SFS_WS_BUSY     2

////////////////////////////////////////////////////////////
// support code

bool
sfs_block_is_journal(struct sfs_fs *sfs, uint32_t block)
{
	if (block >= sfs->sfs_sb.sb_journalstart &&
	    block < sfs->sfs_sb.sb_journalstart +
	    		sfs->sfs_sb.sb_journalblocks) {
		return true;
	}
	return false;
}

/*
 * This is only referenced if SFS_VERBOSE_RECOVERY is on.
 */
#ifdef SFS_VERBOSE_RECOVERY
static
const char *
sfs_jphys_recname(unsigned class, unsigned type)
{
	if (class == SFS_JPHYS_CONTAINER) {
		switch (type) {
		    case SFS_JPHYS_INVALID: return "<invalid>";
		    case SFS_JPHYS_PAD: return "pad";
		    case SFS_JPHYS_TRIM: return "trim";
		    default: return "<unknown>";
		}
	}
	else {
		return sfs_jphys_client_recname(type);
	}
}
#endif /* SFS_VERBOSE_RECOVERY */

////////////////////////////////////////////////////////////
// sfs_jposition ops

static
bool
sfs_jposition_eq(const struct sfs_jposition *a, const struct sfs_jposition *b)
{
	return a->jp_jblock == b->jp_jblock &&
		a->jp_blockoffset == b->jp_blockoffset;
}

////////////////////////////////////////////////////////////
// operating mode interface

/*
 * Move to the next journal block. (If we don't need another journal
 * block yet, return without doing anything.)
 *
 * This releases jp_headbuf and switches in jp_nextbuf, and notes that
 * we're the thread that's going to replace jp_nextbuf later. We can't
 * do buffer_get() here, as if it evicts a buffer that might generate
 * a journal entry, which would have no place to go. (And in fact, it
 * would deadlock on the jphys lock before it got that far.)
 */
static
void
sfs_advance_journal(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;

	/*
	 * XXX we have to make sure here that the journal head never
	 * rams into the tail; not just because it'll make a mess but
	 * because it can deadlock.
	 */

	KASSERT(lock_do_i_hold(jp->jp_lock));

	if (jp->jp_headbyte < SFS_BLOCKSIZE) {
		return;
	}
	/* Must not have run off the end. */
	KASSERT(jp->jp_headbyte == SFS_BLOCKSIZE);

	/* Validate the LSN map entry. */
	spinlock_acquire(&jp->jp_lsnmaplock);
	KASSERT(jp->jp_firstlsns[jp->jp_headjblock] == jp->jp_headfirstlsn);
	spinlock_release(&jp->jp_lsnmaplock);

	/* Release the journal head buffer. */
	buffer_release(jp->jp_headbuf);

	/* Move to the next block.*/
	jp->jp_headjblock++;
	if (jp->jp_headjblock == sfs->sfs_sb.sb_journalblocks) {
		jp->jp_headjblock = 0;
	}
	KASSERT(jp->jp_headjblock < sfs->sfs_sb.sb_journalblocks);
	jp->jp_headbyte = 0;
	jp->jp_headfirstlsn = jp->jp_nextlsn;

	/* Take jp_nextbuf and promise to replace it. */
	KASSERT(jp->jp_nextbuf != NULL);
	KASSERT(jp->jp_gettingnext == NULL);
	jp->jp_headbuf = jp->jp_nextbuf;
	jp->jp_nextbuf = NULL;
	jp->jp_gettingnext = curthread;
	buffer_mark_valid(jp->jp_headbuf);

	/* Update the LSN map. */
	spinlock_acquire(&jp->jp_lsnmaplock);
	if (jp->jp_headjblock == jp->jp_oldestjblock) {
		/* XXX this is the in-memory tail, need to check the on-disk tail too */
		panic("sfs: journal head overran journal tail\n");
	}
	jp->jp_firstlsns[jp->jp_headjblock] = jp->jp_headfirstlsn;
	spinlock_release(&jp->jp_lsnmaplock);
}

/*
 * Fetch the next journal head buffer.
 *
 * This releases the jphys lock while it's working, because it's
 * unsafe to call buffer_get while holding it. (See note above.) This
 * has two implications: first, it can only be done *after* journaling
 * (not in the middle of sfs_advance_journal) and second, we need to
 * make sure only one thread tries to do it at once.
 *
 * The way this works is that the thread that turns over the head
 * buffer in sfs_advance_journal is the thread responsible for
 * replacing jp_nextbuf; it says so by setting jp_gettingnext. Then
 * after it finishes the journaling that it's doing, it calls
 * sfs_getnextbuf.
 *
 * Also, in order to prevent running off the end of the current
 * journal head buffer before a new jp_nextbuf is ready, anyone
 * entering sfs_jphys_write while jp_nextbuf is NULL sleeps until we
 * finish here. If we get back there recursively somehow, we'll panic;
 * currently that can't happen, but if it becomes possible we'll need
 * to allow ourselves through there without waiting. If it becomes
 * possible to generate more than a whole block's worth of journal
 * entries from buffer writes triggered by buffer_get... this whole
 * scheme fails and needs to be redesigned.
 */
static
void
sfs_getnextbuf(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	uint32_t nextjblock, nextdiskblock;
	struct buf *buf;
	int result;

	nextjblock = jp->jp_headjblock + 1;
	if (nextjblock == sfs->sfs_sb.sb_journalblocks) {
		nextjblock = 0;
	}
	nextdiskblock = nextjblock + sfs->sfs_sb.sb_journalstart;
	lock_release(jp->jp_lock);

	result = buffer_get_fsmanaged(&sfs->sfs_absfs, nextdiskblock,
				      SFS_BLOCKSIZE, &buf);
	if (result) {
		/*
		 * XXX this really won't do. However, it can only
		 * happen in the following cases:
		 *    - kmalloc failure in sfs_attachbuf
		 *    - kmalloc failure in bufhash_add in buf.c
		 *
		 * The problem is not so much that we couldn't report
		 * an error to the caller; we could (although it's
		 * much nicer if writing to the journal doesn't fail)
		 * ... the problem is that if we can't get the buffer
		 * we have no way to continue operating. If we leave
		 * jp_nextblock NULL, we'll hang and/or panic as soon
		 * as the current journal head buffer fills up.
		 *
		 * We can rig the buffer cache so it doesn't fail in
		 * bufhash_add; IIRC at least some of that logic is
		 * already in place. And we could probably avoid
		 * needing to kmalloc in sfs_attachbuf for journal
		 * buffers; it's convenient to use the same structure
		 * and same flushing mechanism as for regular buffers,
		 * but not necessary. However, these changes will be a
		 * good bit of further hacking, so not yet. XXX.
		 */
		panic("sfs: %s: turning over journal: %s\n",
		      sfs->sfs_sb.sb_volname, strerror(result));
	}
	buffer_mark_valid(buf);
	lock_acquire(jp->jp_lock);
	jp->jp_nextbuf = buf;
	jp->jp_gettingnext = NULL;
	jp->jp_jblockcount++;
	cv_broadcast(jp->jp_nextcv, jp->jp_lock);
}

/*
 * Write some data directly into the journal.
 */
static
void
sfs_put_journal(struct sfs_fs *sfs, sfs_lsn_t lsn, const void *rec, size_t len)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	char *buf;

	KASSERT(lock_do_i_hold(jp->jp_lock));
	KASSERT(jp->jp_headbyte + len <= SFS_BLOCKSIZE);

	KASSERT(lsn >= jp->jp_headfirstlsn);

	buf = buffer_map(jp->jp_headbuf);
	memcpy(buf + jp->jp_headbyte, rec, len);
	buffer_mark_dirty(jp->jp_headbuf);
	jp->jp_headbyte += len;

	sfs_advance_journal(sfs);
}

/*
 * Write a pad record to the end of the current journal block.
 */
static
void
sfs_pad_journal(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	struct sfs_jphys_header hdr;
	sfs_lsn_t lsn;
	size_t len;

	KASSERT(lock_do_i_hold(jp->jp_lock));
	KASSERT(jp->jp_headbyte < SFS_BLOCKSIZE);

	len = SFS_BLOCKSIZE - jp->jp_headbyte;
	if (len >= sizeof(hdr)) {
		lsn = jp->jp_nextlsn++;
		hdr.jh_coninfo = SFS_MKCONINFO(SFS_JPHYS_CONTAINER,
					       SFS_JPHYS_PAD, len, lsn);
		sfs_put_journal(sfs, lsn, &hdr, sizeof(hdr));
		len -= sizeof(hdr);
	}
	else {
		/* padding is implicit; do nothing */
	}

	jp->jp_headbyte += len;
	sfs_advance_journal(sfs);
}

/*
 * Write a journal entry into the physical journal.
 *
 * TAILLSN is the tail LSN to apply to the tail reservation TRES.
 * TRES can be null in cases where no tail reservation is needed
 * (e.g. for checkpoint records); TAILLSN can also be zero, in
 * which case the LSN of the current record is used.
 *
 * CODE is the journal record type code; REC is the record data, which
 * is of length LEN.
 *
 * Takes care of padding and block boundaries. Handles the record
 * header.
 *
 * Does not fail. If something happens while writing to the journal
 * such that we can't get a journal buffer to write into (see above)
 * we panic, as there's not much one can do to continue in that case.
 */
static
sfs_lsn_t
sfs_jphys_write_internal(struct sfs_fs *sfs,
			 void (*callback)(struct sfs_fs *sfs,
					  sfs_lsn_t newlsn,
					  struct sfs_jphys_writecontext *ctx),
			 struct sfs_jphys_writecontext *ctx,
			 unsigned class, unsigned type,
			 const void *rec, size_t len)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	struct sfs_jphys_header hdr;
	sfs_lsn_t lsn;
	size_t totallen;

	KASSERT(len % 2 == 0);

	/* our total length includes a header */
	totallen = len + sizeof(hdr);

	/* lock the journal */
	lock_acquire(jp->jp_lock);

	/*
	 * If the journal head is turning over, wait until it
	 * finishes. If we're the thread that was supposed to fetch
	 * the next head buffer, die. If this starts to happen, we
	 * need to instead be able to pass through here without
	 * sleeping. Doing so should be reasonably safe - if we
	 * recursively generate enough journal records to fill the
	 * current head buffer we'll die in sfs_advance_journal but
	 * that would mean something's badly wrong.
	 */
	while (jp->jp_nextbuf == NULL) {
		KASSERT(jp->jp_gettingnext != curthread);
		cv_wait(jp->jp_nextcv, jp->jp_lock);
	}

	/* If we aren't going to fit, pad the current block and get a new one */
	if (jp->jp_headbyte + totallen > SFS_BLOCKSIZE) {
		sfs_pad_journal(sfs);
	}

	/* Check some limits required by the container logic */
	KASSERT(class == SFS_JPHYS_CONTAINER || class == SFS_JPHYS_CLIENT);
	KASSERT(type < 128);
	KASSERT(totallen <= SFS_BLOCKSIZE);
	KASSERT(totallen % 2 == 0);

	/* Get a LSN and initialize the record header. */
	lsn = jp->jp_nextlsn++;
	hdr.jh_coninfo = SFS_MKCONINFO(class, type, totallen, lsn);

	/* Write the header and the actual log entry. */
	sfs_put_journal(sfs, lsn, &hdr, sizeof(hdr));
	sfs_put_journal(sfs, lsn, rec, len);

	/* Call the callback, if any */
	if (callback != NULL) {
		callback(sfs, lsn, ctx);
	}

	/*
	 * If we turned over the head buffer, get a new nextbuf.
	 * This releases the jphys lock while working so it must come
	 * after all the work that needs to be atomic.
	 */
	if (jp->jp_nextbuf == NULL && jp->jp_gettingnext == curthread) {
		sfs_getnextbuf(sfs);
	}
	KASSERT(jp->jp_nextbuf != NULL);

	/* done with the jphys lock */
	lock_release(jp->jp_lock);

	/* return the LSN we used */
	return lsn;
}

/*
 * External version, that asserts if anyone tries to write
 * record numbers reserved for jphys.
 */
sfs_lsn_t
sfs_jphys_write(struct sfs_fs *sfs,
		void (*callback)(struct sfs_fs *sfs,
				 sfs_lsn_t newlsn,
				 struct sfs_jphys_writecontext *ctx),
		struct sfs_jphys_writecontext *ctx,
		unsigned code, const void *rec, size_t len)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;

	/* Must get to operating mode before adding journal entries. */
	KASSERT(jp->jp_operatingmode);

	return sfs_jphys_write_internal(sfs, callback, ctx, SFS_JPHYS_CLIENT,
					code, rec, len);
}

/*
 * Make sure the journal records up to and including the given LSN
 * are written to disk; write them out if necessary.
 *
 * There are two ways to get here:
 *
 * 1. When the buffer cache writes out a buffer, it calls
 * sfs_writeblock; that checks higher-level data structures and
 * calls sfs_jphys_flush() if necessary to maintain the ordering of
 * the journal or the write-ahead invariant required by recovery.
 *
 * Journal blocks *must* be written out in order, because not doing so
 * violates assumptions made by the recovery code.
 *
 * 2. An explicit sync call goes through sfs_sync, which calls
 * sfs_jphys_flush() twice, once for write-ahead before writing out
 * the free block bitmap, and again at the end to write out the
 * checkpoint it made.
 *
 * When we get here from sfs_writeblock, we are always holding at
 * least one buffer, namely the one sfs_writeblock is supposed to
 * write out. That means that any locks acquired in this function must
 * come *after* buffer locks; that is, one can't buffer_get while
 * holding any such lock. This means both jp_lock and jp_lsnmaplock.
 *
 * Other considerations:
 *
 * - We're called with LSNS but we need to do I/O in terms of blocks,
 * so we need to be able to figure out which journal block a given LSN
 * went into. Since we don't require records to all be the same size,
 * we have to maintain a mapping. jp->firstlsns[] contains (for each
 * journal block) the first LSN in that block. For now it's just a
 * (large) array indexed by journal block number. As most of the time
 * most of it will be zero, it would probably be better to come up with
 * a more compact representation. (XXX)
 *
 * - If a journal buffer is written by the syncer, rather than being
 * written explicitly from here, it won't get invalidated and it will
 * hang around until the buffer cache decides to discard it. This is
 * silly, but not a major problem.
 *
 * - If the LSN we want to write out is in the current journal head
 * block, we need to pad the current head block and get a new one.
 * We do this first.
 */
int
sfs_jphys_flush(struct sfs_fs *sfs, sfs_lsn_t lsn)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	uint32_t jblock, headjblock, diskblock;
	sfs_lsn_t headfirstlsn;
	int result;

	if (lsn == 0) {
		/* this can happen during recovery; don't choke on it */
		return 0;
	}

	KASSERT(jp->jp_operatingmode);

	lock_acquire(jp->jp_lock);

	KASSERT(lsn < jp->jp_nextlsn);

	if (lsn >= jp->jp_headfirstlsn && jp->jp_headbyte > 0) {
		/*
		 * We will need to flush out the current journal head;
		 * advance the head.
		 */
		sfs_pad_journal(sfs);
		if (jp->jp_nextbuf == NULL && jp->jp_gettingnext == curthread){
			sfs_getnextbuf(sfs);
		}
	}

	/*
	 * If someone advances the head further while we're working, we
	 * don't actually care, so grab the current values and release
	 * jp_lock.
	 */
	headjblock = jp->jp_headjblock;
	headfirstlsn = jp->jp_headfirstlsn;
	lock_release(jp->jp_lock);

	/* Lock the state */
	spinlock_acquire(&jp->jp_lsnmaplock);
	jblock = jp->jp_oldestjblock;
	while (1) {
		if (lsn < jp->jp_firstlsns[jblock]) {
			/* flushed as far as we need */
			break;
		}
		/* cannot reach the head */
		KASSERT(jblock != headjblock);

		/*
		 * Unlock so that sfs_writeblock can call back into
		 * here to update jp_oldestjblock after doing the
		 * write.
		 *
		 * A number of races can arise here because of this
		 * (e.g. multiple threads might all try to write the
		 * same block out) -- this can get very hairy if
		 * tackled head-on. Instead we take advantage of
		 * buffer_flush being idempotent (it does nothing if
		 * the buffer is clean or no longer present) and that
		 * makes all these problems go away.
		 */
		spinlock_release(&jp->jp_lsnmaplock);

		/* write the buffer out */
		diskblock = sfs->sfs_sb.sb_journalstart + jblock;
		result = buffer_flush(&sfs->sfs_absfs, diskblock,
				      SFS_BLOCKSIZE);
		if (result) {
			/* Oopsey. */
			panic("sfs: writing journal buffer: %s\n",
			      strerror(result));
		}

		/* now invalidate the buffer too; don't need it any more */
		buffer_drop(&sfs->sfs_absfs, diskblock, SFS_BLOCKSIZE);

		/* Get the spinlock again and go on to the next block */
		spinlock_acquire(&jp->jp_lsnmaplock);
		jblock++;
		if (jblock >= sfs->sfs_sb.sb_journalblocks) {
			jblock = 0;
		}
	}

	KASSERT(lsn < headfirstlsn);

	spinlock_release(&jp->jp_lsnmaplock);
	return 0;
}

/*
 * Flush the journal up to but not including a particular journal
 * block DISKBLOCK.
 *
 * DISKBLOCK is the *disk* block number (not the journal block number)
 * because that's what's readily available in sfs_writeblock where
 * this is called.
 */
int
sfs_jphys_flushforjournalblock(struct sfs_fs *sfs, daddr_t diskblock)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	uint32_t jblock;
	sfs_lsn_t lsn;

	/* figure out which journal block it is */
	jblock = diskblock - sfs->sfs_sb.sb_journalstart;
	KASSERT(jblock < sfs->sfs_sb.sb_journalblocks);

	/* look up the equivalent LSN */
	/*
	 * (XXX this is not the right way to do this; but one thing at
	 * a time)
	 */
	spinlock_acquire(&jp->jp_lsnmaplock);
	/*
	 * We need to flush up to but NOT including jblock
	 * (XXX: this is not clear from the function or variable naming)
	 * so flush up to AND including the LSN before the first one
	 * on the block.
	 *
	 * XXX: this will blow up badly if the first LSN is 0, but
	 * that's not supposed to be possible.
	 */
	lsn = jp->jp_firstlsns[jblock] - 1;
	spinlock_release(&jp->jp_lsnmaplock);

	return sfs_jphys_flush(sfs, lsn);
}

/*
 * Flush the whole journal.
 */ 
int
sfs_jphys_flushall(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	sfs_lsn_t nextlsn;
	int result;

	lock_acquire(jp->jp_lock);
	nextlsn = jp->jp_nextlsn;
	lock_release(jp->jp_lock);

	result = sfs_jphys_flush(sfs, nextlsn - 1);
	if (result) {
		return result;
	}
	
	return 0;
}

/*
 * Mark that a particular block in the journal has been written.
 * DISKBLOCK is the *disk* block number (not the journal block number)
 * because that's what's readily available in sfs_writeblock where
 * this is called.
 */
void
sfs_wrote_journal_block(struct sfs_fs *sfs, daddr_t diskblock)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	uint32_t jblock;

	/* figure out which journal block it is */
	jblock = diskblock - sfs->sfs_sb.sb_journalstart;
	KASSERT(jblock < sfs->sfs_sb.sb_journalblocks);

	spinlock_acquire(&jp->jp_lsnmaplock);
	KASSERT(jblock == jp->jp_oldestjblock);
	jp->jp_oldestjblock++;
	if (jp->jp_oldestjblock >= sfs->sfs_sb.sb_journalblocks) {
		jp->jp_oldestjblock = 0;
	}
	spinlock_release(&jp->jp_lsnmaplock);
}

/*
 * Fetch the current next-LSN. Note that more records may be
 * added before the caller sees the value, so the safe uses
 * of the value are very limited.
 */
sfs_lsn_t
sfs_jphys_peeknextlsn(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	sfs_lsn_t nextlsn;

	lock_acquire(jp->jp_lock);
	nextlsn = jp->jp_nextlsn;
	lock_release(jp->jp_lock);

	return nextlsn;
}

void
sfs_jphys_trim(struct sfs_fs *sfs, sfs_lsn_t taillsn)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	struct sfs_jphys_trim rec;

	KASSERT(jp->jp_operatingmode);

	rec.jt_taillsn = taillsn;
	sfs_jphys_write_internal(sfs, 0, NULL,
				 SFS_JPHYS_CONTAINER, SFS_JPHYS_TRIM,
				 &rec, sizeof(rec));
}

uint32_t
sfs_jphys_getjblockcount(struct sfs_jphys *jp)
{
	uint32_t ret;

	KASSERT(jp->jp_operatingmode);

	/*
	 * In a production kernel one would probably use atomic
	 * operations for jblockcount.
	 */
	lock_acquire(jp->jp_lock);
	ret = jp->jp_jblockcount;
	lock_release(jp->jp_lock);

	return ret;
}

void
sfs_jphys_clearjblockcount(struct sfs_jphys *jp)
{
	KASSERT(jp->jp_operatingmode);

	lock_acquire(jp->jp_lock);
	jp->jp_jblockcount = 0;
	lock_release(jp->jp_lock);
}

////////////////////////////////////////////////////////////
// journal iterator (recovery mode) interface

/*
 * Journal iteration state.
 *
 * ji_tailpos is the oldest record the iteration covers.
 * ji_headpos is one past the newest record the iteration covers.
 *
 * These can be (often are) the same position; that iterates the
 * entire journal.
 *
 * Iterating forward goes from tail to head; backward goes from head
 * to tail.
 *
 * Rewinding to the tail end sets the position to tailpos; rewinding
 * to the head end sets the position to headpos and then backs up by
 * one so as to be *on* the newest record.
 *
 * Moving forward *to* headpos, or backward *from* tailpos, does not
 * actually move but instead sets ji_done.
 *
 * There is no way to set headpos and tailpos so that the iteration
 * seems empty; however, that's ok as the journal can never be fully
 * empty. (There must always be at least one checkpoint.)
 */
struct sfs_jiter {
	/* iteration bounds */
	struct sfs_jposition ji_headpos;
	struct sfs_jposition ji_tailpos;

	/* current position */
	struct sfs_jposition ji_pos;

	/* state flags */
	bool ji_read;		/* true if current record has been read in */
	bool ji_done;		/* true if we've bumped into either end */
	bool ji_seeall;		/* true to show container-level records */

	/* buffer for current journal block */
	struct buf *ji_buf;

	/* current record (valid if ji_read is true) */
	unsigned ji_class;
	unsigned ji_type;
	unsigned ji_len;
	sfs_lsn_t ji_lsn;
};

/*
 * Create an iterator.
 *
 * The iterator covers records between TAILPOS and HEADPOS, including
 * TAILPOS but not HEADPOS as described above. To scan the entire
 * physical journal, set TAILPOS == HEADPOS.
 */
static
struct sfs_jiter *
sfs_jiter_create(struct sfs_fs *sfs,
		 const struct sfs_jposition *tailpos,
		 const struct sfs_jposition *headpos,
		 bool seeall)
{
	struct sfs_jiter *ji;

	(void)sfs;

	ji = kmalloc(sizeof(*ji));
	if (ji == NULL) {
		return NULL;
	}

	ji->ji_tailpos = *tailpos;
	ji->ji_headpos = *headpos;

	/* start at the tail by default */
	ji->ji_pos = *tailpos;

	ji->ji_buf = NULL;

	ji->ji_read = false;
	ji->ji_done = false;
	ji->ji_seeall = seeall;

	ji->ji_class = SFS_JPHYS_CONTAINER;
	ji->ji_type = SFS_JPHYS_INVALID;
	ji->ji_len = 0;
	ji->ji_lsn = 0;

	return ji;
}

/*
 * Check if done iterating.
 */
bool
sfs_jiter_done(struct sfs_jiter *ji)
{
	return ji->ji_done;
}

/*
 * Get current position.
 */
static
void
sfs_jiter_pos(struct sfs_jiter *ji, struct sfs_jposition *jp)
{
	*jp = ji->ji_pos;
}

/*
 * Get the jblock number (block index in journal) of the current
 * position.
 */
static
uint32_t
sfs_jiter_jblock(struct sfs_jiter *ji)
{
	return ji->ji_pos.jp_jblock;
}

/*
 * Get the block offset of the current position.
 */
static
unsigned
sfs_jiter_blockoffset(struct sfs_jiter *ji)
{
	return ji->ji_pos.jp_blockoffset;
}

/*
 * Get type class of current record.
 */
static
unsigned
sfs_jiter_class(struct sfs_jiter *ji)
{
	KASSERT(!ji->ji_done);
	KASSERT(ji->ji_read);

	return ji->ji_class;
}

/*
 * Get type of current record.
 */
unsigned
sfs_jiter_type(struct sfs_jiter *ji)
{
	KASSERT(!ji->ji_done);
	KASSERT(ji->ji_read);

	return ji->ji_type;
}

/*
 * Get LSN of current record. Might be 0; zero LSNs should be ignored
 * even (especially) if they appear out of sequence.
 */
sfs_lsn_t
sfs_jiter_lsn(struct sfs_jiter *ji)
{
	KASSERT(!ji->ji_done);
	KASSERT(ji->ji_read);

	return ji->ji_lsn;
}

/*
 * Get the current record, without the header.
 */
void *
sfs_jiter_rec(struct sfs_jiter *ji, size_t *len_ret)
{
	unsigned offset;

	KASSERT(!ji->ji_done);
	KASSERT(ji->ji_read);
	KASSERT(ji->ji_buf != NULL);
	KASSERT(ji->ji_len >= sizeof(struct sfs_jphys_header));

	*len_ret = ji->ji_len - sizeof(struct sfs_jphys_header);
	offset = ji->ji_pos.jp_blockoffset + sizeof(struct sfs_jphys_header);
	return (char *)buffer_map(ji->ji_buf) + offset;
}

/*
 * Ensure that we have a buffer for the current journal block.
 * Internal.
 */
static
int
sfs_jiter_getbuf(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	int result;

	if (ji->ji_buf != NULL) {
		return 0;
	}
	result = buffer_read(&sfs->sfs_absfs,
			     sfs->sfs_sb.sb_journalstart +
			     ji->ji_pos.jp_jblock,
			     SFS_BLOCKSIZE, &ji->ji_buf);
	if (result) {
		SAY("sfs_jiter_getbuf: buffer_read: %s\n",
		    strerror(result));
	}
	return result;
}

/*
 * Read the current record.
 */
static
int
sfs_jiter_read(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	char *ptr;
	struct sfs_jphys_header jh;
	int result;

	KASSERT(!ji->ji_done);

	if (ji->ji_read) {
		return 0;
	}
	result = sfs_jiter_getbuf(sfs, ji);
	if (result) {
		return result;
	}
	ptr = buffer_map(ji->ji_buf);
	KASSERT(ji->ji_pos.jp_blockoffset + sizeof(jh) <= SFS_BLOCKSIZE);
	memcpy(&jh, ptr + ji->ji_pos.jp_blockoffset, sizeof(jh));
	if (jh.jh_coninfo == 0) {
		ji->ji_class = SFS_JPHYS_CONTAINER;
		ji->ji_type = SFS_JPHYS_PAD;
		ji->ji_len  = sizeof(jh);
		ji->ji_lsn = 0;
	}
	else {
		ji->ji_class = SFS_CONINFO_CLASS(jh.jh_coninfo);
		ji->ji_type = SFS_CONINFO_TYPE(jh.jh_coninfo);
		ji->ji_len = SFS_CONINFO_LEN(jh.jh_coninfo);
		ji->ji_lsn = SFS_CONINFO_LSN(jh.jh_coninfo);
	}
	ji->ji_read = true;

	if (ji->ji_len < sizeof(jh)) {
		kprintf("sfs: %s: runt journal record, length %u, "
			"jblock %u offset %u\n",
			sfs->sfs_sb.sb_volname, ji->ji_len,
			ji->ji_pos.jp_jblock, ji->ji_pos.jp_blockoffset);
		return EFTYPE;
	}

	if (ji->ji_pos.jp_blockoffset + ji->ji_len > SFS_BLOCKSIZE) {
		kprintf("sfs: %s: journal record runs off end of block, "
			"jblock %u offset %u\n",
			sfs->sfs_sb.sb_volname,
			ji->ji_pos.jp_jblock, ji->ji_pos.jp_blockoffset);
		return EFTYPE;
	}

	if (ji->ji_class == SFS_JPHYS_CONTAINER &&
	    ji->ji_type == SFS_JPHYS_INVALID) {
		kprintf("sfs: %s: invalid entry in journal, "
			"jblock %u offset %u\n",
			sfs->sfs_sb.sb_volname,
			ji->ji_pos.jp_jblock, ji->ji_pos.jp_blockoffset);
		return EFTYPE;
	}

	return 0;
}

/*
 * Move to the next record.
 *
 * We are done if the *next* record position is the head position; so
 * compute the next position before changing anything in the iterator.
 * (That way the iterator position on reaching the end remains well
 * defined.)
 */
static
int
sfs_jiter_one_next(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	struct sfs_jposition pos;
	bool changebuf;
	int result;

	KASSERT(ji->ji_read);
	pos = ji->ji_pos;
	changebuf = false;

	/* Compute the new position */

	pos.jp_blockoffset += ji->ji_len;
	KASSERT(pos.jp_blockoffset <= SFS_BLOCKSIZE);

	if (pos.jp_blockoffset + sizeof(struct sfs_jphys_header) >
	    SFS_BLOCKSIZE) {
		/* If no room for another header, skip the rest of the block */
		pos.jp_blockoffset = SFS_BLOCKSIZE;
	}

	if (pos.jp_blockoffset == SFS_BLOCKSIZE) {
		pos.jp_blockoffset = 0;
		pos.jp_jblock++;
		if (pos.jp_jblock == sfs->sfs_sb.sb_journalblocks) {
			pos.jp_jblock = 0;
		}
		changebuf = true;
	}

	/* Check for being done */
	if (sfs_jposition_eq(&pos, &ji->ji_headpos)) {
		ji->ji_done = true;
		return 0;
	}

	/* Apply the new position */
	ji->ji_read = false;
	ji->ji_pos = pos;
	if (changebuf && ji->ji_buf != NULL) {
		buffer_release(ji->ji_buf);
		ji->ji_buf = NULL;
	}

	/* If we were done, we aren't any more */
	ji->ji_done = false;

	/* Read the record under the iterator. */
	result = sfs_jiter_read(sfs, ji);
	if (result) {
		return result;
	}

	return 0;
}

/*
 * Move to the next record and skip over internal records if necessary.
 */
int
sfs_jiter_next(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	int result;

	do {
		result = sfs_jiter_one_next(sfs, ji);
		if (result) {
			return result;
		}
	} while (!ji->ji_done &&
		 !ji->ji_seeall && ji->ji_class == SFS_JPHYS_CONTAINER);
	return 0;
}

/*
 * The guts of moving to the previous record.
 *
 * Note that unlike with next, the done test comes first: going
 * backwards we are done if the current position is equal to the tail
 * position. So we don't have to worry about altering the iterator
 * state - by the time we get to this function we know we're backing
 * up.
 *
 * Note that if we fail, the iterator state is undefined. This is
 * not optimal; but read errors on the journal aren't recoverable. The
 * only reason we don't just panic is that we know we're in the middle
 * of mounting the volume; the caller should be able to unwind that so
 * the system can continue running.
 */
static
int
sfs_jiter_one_prev(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	char *ptr;
	struct sfs_jphys_header jh;
	unsigned offset, prevoffset;
	size_t len;
	int result;

	KASSERT(ji->ji_pos.jp_blockoffset < SFS_BLOCKSIZE);

	/* make gcc happy */
	prevoffset = 0;

	if (ji->ji_pos.jp_blockoffset == 0) {
		ji->ji_pos.jp_blockoffset = SFS_BLOCKSIZE;
		if (ji->ji_pos.jp_jblock == 0) {
			ji->ji_pos.jp_jblock = sfs->sfs_sb.sb_journalblocks;
		}
		ji->ji_pos.jp_jblock--;
		if (ji->ji_buf != NULL) {
			buffer_release(ji->ji_buf);
			ji->ji_buf = NULL;
		}
	}

	result = sfs_jiter_getbuf(sfs, ji);
	if (result) {
		return result;
	}
	ptr = buffer_map(ji->ji_buf);

	/* flip through the block to move backwards 1; ugly */
	offset = 0;
	KASSERT(ji->ji_pos.jp_blockoffset > 0);
	while (offset < ji->ji_pos.jp_blockoffset) {
		if (offset + sizeof(jh) > SFS_BLOCKSIZE) {
			/*
			 * If there isn't room for a header, it's
			 * waste space at the end of the block and we
			 * should ignore it.
			 */
			break;
		}
		prevoffset = offset;
		memcpy(&jh, ptr + offset, sizeof(jh));
		len = SFS_CONINFO_LEN(jh.jh_coninfo);
		if (len == 0) {
			KASSERT(jh.jh_coninfo == 0);
			len = sizeof(jh);
		}
		offset += len;
	}
	ji->ji_pos.jp_blockoffset = prevoffset;
	ji->ji_read = false;

	/* If we were done, we aren't any more */
	ji->ji_done = false;

	/* Read the record under the iterator. */
	result = sfs_jiter_read(sfs, ji);
	if (result) {
		return result;
	}

	return 0;
}

/*
 * Move to the previous record, skipping over internal records if
 * necessary. Internal version that omits the initial done test.
 *
 * This allows it to be used to move back from the head position, even
 * if the head position is equal to the tail position; this is
 * necessary for seeking to the head end.
 */
static
int
sfs_jiter_doprev(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	int result;

	do {
		result = sfs_jiter_one_prev(sfs, ji);
		if (result) {
			return result;
		}

		if (sfs_jposition_eq(&ji->ji_pos, &ji->ji_tailpos)) {
			ji->ji_done = true;
			return 0;
		}
	} while (!ji->ji_seeall && ji->ji_class == SFS_JPHYS_CONTAINER);
	return 0;
}

/*
 * Move to the previous record, skipping over internal records if
 * necessary.
 */
int
sfs_jiter_prev(struct sfs_fs *sfs, struct sfs_jiter *ji)
{

	if (sfs_jposition_eq(&ji->ji_pos, &ji->ji_tailpos)) {
		ji->ji_done = true;
		return 0;
	}

	return sfs_jiter_doprev(sfs, ji);
}

/*
 * Seek to the head end of the journal (for scanning backward)
 */
int
sfs_jiter_seekhead(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	int result;

	ji->ji_pos = ji->ji_headpos;

	/*
	 * Back up one, using the internal interface that lets us move
	 * across the head/tail boundary. This also reads the record,
	 * and if we're hiding internal records will continue backing
	 * up until we find a client record or hit the end.
	 */
	result = sfs_jiter_doprev(sfs, ji);
	if (result) {
		return result;
	}

	return 0;
}

/*
 * Seek to the tail end of the journal (for scanning forward)
 */
int
sfs_jiter_seektail(struct sfs_fs *sfs, struct sfs_jiter *ji)
{
	int result;

	ji->ji_pos = ji->ji_tailpos;

	/* We don't need to advance, so just read the record. */
	result = sfs_jiter_read(sfs, ji);
	if (result) {
		return result;
	}

	/* If it's an internal record, move forward if necessary. */
	if (!ji->ji_seeall && ji->ji_class == SFS_JPHYS_CONTAINER) {
		result = sfs_jiter_next(sfs, ji);
		if (result) {
			return result;
		}
	}

	return 0;
}

/*
 * Create an external forward iterator. This scans from the tail we
 * found to the head we found.
 */
int
sfs_jiter_fwdcreate(struct sfs_fs *sfs, struct sfs_jiter **ji_ret)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	struct sfs_jiter *ji;
	int result;

	KASSERT(jp->jp_recoverymode);

	ji = sfs_jiter_create(sfs,
			      &jp->jp_recov_tailpos, &jp->jp_recov_headpos,
			      false /*seeall*/);
	if (ji == NULL) {
		return ENOMEM;
	}

	result = sfs_jiter_seektail(sfs, ji);
	if (result) {
		sfs_jiter_destroy(ji);
		return result;
	}

	*ji_ret = ji;
	return 0;
}

/*
 * Create an external backward iterator. This scans from the head we
 * found to the tail we found.
 */
int
sfs_jiter_revcreate(struct sfs_fs *sfs, struct sfs_jiter **ji_ret)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	struct sfs_jiter *ji;
	int result;

	KASSERT(jp->jp_recoverymode);

	ji = sfs_jiter_create(sfs,
			      &jp->jp_recov_tailpos, &jp->jp_recov_headpos,
			      false /*seeall*/);
	if (ji == NULL) {
		return ENOMEM;
	}

	result = sfs_jiter_seekhead(sfs, ji);
	if (result) {
		sfs_jiter_destroy(ji);
		return result;
	}

	*ji_ret = ji;
	return 0;
}

/*
 * Clean up after iterating.
 */
void
sfs_jiter_destroy(struct sfs_jiter *ji)
{
	if (ji->ji_buf != NULL) {
		buffer_release(ji->ji_buf);
		ji->ji_buf = NULL;
	}
	kfree(ji);
}

////////////////////////////////////////////////////////////
// container-level recovery

/*
 * Look for the head. If we see at least one checkpoint in the process,
 * provide the tail LSN and the position to start looking for the tail.
 * Always provide the head position and head LSN.
 *
 * FUTURE: find the head by binary search.
 */
static
int
sfs_scan_for_head(struct sfs_fs *sfs,
		  struct sfs_jposition *tailsearchpos_ret,
		  sfs_lsn_t *taillsn_ret,
		  struct sfs_jposition *headpos_ret,
		  sfs_lsn_t *headlsn_ret)
{
	struct sfs_jiter *ji;
	struct sfs_jposition startpos;
	bool first;
	sfs_lsn_t firstlsn;
	sfs_lsn_t prevlsn;
	sfs_lsn_t thislsn;
	unsigned class, type;
	void *rec;
	size_t reclen;
	struct sfs_jphys_trim jt;
	int result;

	/* Scan forward from the physical beginning. */
	first = true;
	firstlsn = 0;
	prevlsn = 0;
	*taillsn_ret = 0;
	startpos.jp_jblock = 0;
	startpos.jp_blockoffset = 0;

	ji = sfs_jiter_create(sfs, &startpos, &startpos, true/*seeall*/);
	if (ji == NULL) {
		return ENOMEM;
	}

	result = sfs_jiter_seektail(sfs, ji);
	if (result) {
		sfs_jiter_destroy(ji);
		return result;
	}

	while (!sfs_jiter_done(ji)) {
		result = sfs_jiter_read(sfs, ji);
		if (result) {
			sfs_jiter_destroy(ji);
			return result;
		}

		class = sfs_jiter_class(ji);
		type = sfs_jiter_type(ji);
		thislsn = sfs_jiter_lsn(ji);
		rec = sfs_jiter_rec(ji, &reclen);

		SAY("[%u.%u] %llu: %s type %u (%s)\n", ji->ji_pos.jp_jblock,
		    ji->ji_pos.jp_blockoffset, thislsn,
		    class == SFS_JPHYS_CONTAINER ? "container" : "client",
		    type, sfs_jphys_recname(class, type));
		UNSAID(class);

		if (first && thislsn != 0) {
			firstlsn = thislsn;
			first = false;
		}

		if (prevlsn != 0 && thislsn < prevlsn) {
			/* found the head */
			if (sfs_jiter_blockoffset(ji) != 0) {
				kprintf("sfs: %s: journal head within block, "
					"block %u offset %u\n",
					sfs->sfs_sb.sb_volname,
					sfs_jiter_jblock(ji),
					sfs_jiter_blockoffset(ji));
				sfs_jiter_destroy(ji);
				return EFTYPE;
			}
			sfs_jiter_pos(ji, headpos_ret);
			*headlsn_ret = prevlsn + 1;
			sfs_jiter_destroy(ji);
			return 0;
		}

		if (type == SFS_JPHYS_TRIM) {
			if (reclen != sizeof(jt)) {
				kprintf("sfs: %s: wrong size trim "
					"record, block %u offset %u\n",
					sfs->sfs_sb.sb_volname,
					sfs_jiter_jblock(ji),
					sfs_jiter_blockoffset(ji));
				sfs_jiter_destroy(ji);
				return EFTYPE;
			}
			memcpy(&jt, rec, sizeof(jt));

			/*
			 * Search should include the checkpoint
			 * record, so advance the iterator now; then
			 * if we get the position it will all work
			 * properly.
			 */
			result = sfs_jiter_next(sfs, ji);
			if (result) {
				sfs_jiter_destroy(ji);
				return result;
			}

			if (jt.jt_taillsn < firstlsn) {
				tailsearchpos_ret->jp_jblock = 0;
				tailsearchpos_ret->jp_blockoffset = 0;
			}
			else {
				sfs_jiter_pos(ji, tailsearchpos_ret);
			}
			*taillsn_ret = jt.jt_taillsn;
		}
		else {
			result = sfs_jiter_next(sfs, ji);
			if (result) {
				sfs_jiter_destroy(ji);
				return result;
			}
		}

		prevlsn = thislsn;
	}
	sfs_jiter_destroy(ji);

	/*
	 * We found no head. It must have aligned exactly with the
	 * rollover point.
	 */
	headpos_ret->jp_jblock = 0;
	headpos_ret->jp_blockoffset = 0;
	*headlsn_ret = prevlsn + 1;
	return 0;
}

/*
 * Scan backwards for a checkpoint. Return the tail LSN from the
 * checkpoint, and the position to start looking for the tail at.
 */
static
int
sfs_scan_for_checkpoint(struct sfs_fs *sfs,
			struct sfs_jposition *tailsearchpos_ret,
			sfs_lsn_t *taillsn_ret)
{
	struct sfs_jposition startpos;
	struct sfs_jiter *ji;
	unsigned class, type;
	sfs_lsn_t thislsn;
	void *rec;
	size_t reclen;
	struct sfs_jphys_trim jt;
	int result;

	/*
	 * If there was a checkpoint between the physical beginning
	 * and the head, we would have found it already. So scan
	 * backward from the physical end.
	 */
	startpos.jp_jblock = 0;
	startpos.jp_blockoffset = 0;
	ji = sfs_jiter_create(sfs, &startpos, &startpos, true /*seeall*/);
	if (ji == NULL) {
		return ENOMEM;
	}

	result = sfs_jiter_seekhead(sfs, ji);
	if (result) {
		sfs_jiter_destroy(ji);
		return result;
	}

	while (!sfs_jiter_done(ji)) {
		result = sfs_jiter_read(sfs, ji);
		if (result) {
			sfs_jiter_destroy(ji);
			return result;
		}

		class = sfs_jiter_class(ji);
		type = sfs_jiter_type(ji);
		thislsn = sfs_jiter_lsn(ji);
		rec = sfs_jiter_rec(ji, &reclen);

		SAY("[%u.%u] %llu: %s type %u (%s)\n", ji->ji_pos.jp_jblock,
		    ji->ji_pos.jp_blockoffset, thislsn,
		    class == SFS_JPHYS_CONTAINER ? "container" : "client",
		    type, sfs_jphys_recname(class, type));
		UNSAID(thislsn);
		UNSAID(class);

		if (type == SFS_JPHYS_TRIM) {
			if (reclen != sizeof(jt)) {
				kprintf("sfs: %s: wrong size trim "
					"record, block %u offset %u\n",
					sfs->sfs_sb.sb_volname,
					sfs_jiter_jblock(ji),
					sfs_jiter_blockoffset(ji));
				sfs_jiter_destroy(ji);
				return EFTYPE;
			}

			memcpy(&jt, rec, sizeof(jt));
			*taillsn_ret = jt.jt_taillsn;
			sfs_jiter_pos(ji, tailsearchpos_ret);

			sfs_jiter_destroy(ji);
			return 0;
		}

		result = sfs_jiter_prev(sfs, ji);
		if (result) {
			sfs_jiter_destroy(ji);
			return result;
		}
	}
	sfs_jiter_destroy(ji);

	kprintf("sfs: %s: no checkpoint found\n",
		sfs->sfs_sb.sb_volname);
	return EFTYPE;
}

/*
 * Scan backwards from tailsearchpos to find the record with LSN
 * taillsn, and return its physical position.
 */
static
int
sfs_scan_for_tail(struct sfs_fs *sfs,
		  const struct sfs_jposition *tailsearchpos,
		  sfs_lsn_t taillsn,
		  struct sfs_jposition *tailpos_ret)
{
	struct sfs_jiter *ji;
	unsigned class, type;
	sfs_lsn_t thislsn;
	int result;

	ji = sfs_jiter_create(sfs, tailsearchpos, tailsearchpos,
			      true /*seeall*/);
	if (ji == NULL) {
		return ENOMEM;
	}

	result = sfs_jiter_seekhead(sfs, ji);
	if (result) {
		sfs_jiter_destroy(ji);
		return result;
	}

	while (!sfs_jiter_done(ji)) {
		result = sfs_jiter_read(sfs, ji);
		if (result) {
			sfs_jiter_destroy(ji);
			return result;
		}

		class = sfs_jiter_class(ji);
		type = sfs_jiter_type(ji);
		thislsn = sfs_jiter_lsn(ji);

		SAY("[%u.%u] %llu: %s type %u (%s)\n", ji->ji_pos.jp_jblock,
		    ji->ji_pos.jp_blockoffset, thislsn,
		    class == SFS_JPHYS_CONTAINER ? "container" : "client",
		    type, sfs_jphys_recname(class, type));
		UNSAID(class);
		UNSAID(type);

		if (thislsn == taillsn) {
			sfs_jiter_pos(ji, tailpos_ret);
			sfs_jiter_destroy(ji);
			return 0;
		}

		result = sfs_jiter_prev(sfs, ji);
		if (result) {
			sfs_jiter_destroy(ji);
			return result;
		}
	}
	sfs_jiter_destroy(ji);

	kprintf("sfs: %s: tail LSN %llu not found -- overwritten?\n",
		sfs->sfs_sb.sb_volname, (unsigned long long)taillsn);
	return EFTYPE;
}

/*
 * Overall function for container-level recovery.
 */
int
sfs_jphys_recover(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	struct sfs_jposition tailsearchpos;
	sfs_lsn_t headlsn, taillsn;
	int result;

	KASSERT(!jp->jp_physrecovered);

	reserve_buffers(SFS_BLOCKSIZE);

	SAY("Scanning to find the head...\n");
	result = sfs_scan_for_head(sfs, &tailsearchpos, &taillsn,
				   &jp->jp_recov_headpos, &headlsn);
	if (result) {
		goto out;
	}

	SAY("[%u.%u] %llu: HEAD\n",
	    jp->jp_recov_headpos.jp_jblock,
	    jp->jp_recov_headpos.jp_blockoffset,
	    headlsn);

	/* must have a head now */
	KASSERT(headlsn != 0);

	/* if we haven't got the tail lsn, keep looking */
	if (taillsn == 0) {
		SAY("Scanning to find a checkpoint...\n");
		result = sfs_scan_for_checkpoint(sfs, &tailsearchpos,
						 &taillsn);
		if (result) {
			goto out;
		}
	}

	SAY("[?.?] %llu: TAIL\n", taillsn);

	/* must have a tail now */
	KASSERT(taillsn != 0);

	/* find the tail's physical position */
	SAY("Scanning to find the tail position...\n");
	result = sfs_scan_for_tail(sfs, &tailsearchpos, taillsn,
				   &jp->jp_recov_tailpos);
	if (result) {
		goto out;
	}

	SAY("[%u.%u] %llu: TAIL\n",
	    jp->jp_recov_tailpos.jp_jblock,
	    jp->jp_recov_tailpos.jp_blockoffset,
	    taillsn);

	/* head position should be block-aligned */
	KASSERT(jp->jp_recov_headpos.jp_blockoffset == 0);

	jp->jp_headjblock = jp->jp_recov_headpos.jp_jblock;
	jp->jp_headbyte = jp->jp_recov_headpos.jp_blockoffset;
	jp->jp_headfirstlsn = headlsn;

	jp->jp_nextlsn = headlsn;

	jp->jp_physrecovered = true;

out:
	unreserve_buffers(SFS_BLOCKSIZE);
	return result;
}

////////////////////////////////////////////////////////////
// startup, shutdown, and state transition

struct sfs_jphys *
sfs_jphys_create(void)
{
	struct sfs_jphys *jp;

	jp = kmalloc(sizeof(*jp));
	if (jp == NULL) {
		return NULL;
	}
	jp->jp_physrecovered = false;
	jp->jp_recoverymode = false;
	jp->jp_operatingmode = false;

	jp->jp_lock = lock_create("sfs_jphys");
	if (jp->jp_lock == NULL) {
		kfree(jp);
		return NULL;
	}

	jp->jp_headbuf = NULL;
	jp->jp_nextbuf = NULL;
	jp->jp_gettingnext = NULL;
	jp->jp_nextcv = cv_create("sfs_nextbuf");
	if (jp->jp_nextcv == NULL) {
		lock_destroy(jp->jp_lock);
		kfree(jp);
		return NULL;
	}

	jp->jp_headjblock = 0;
	jp->jp_headbyte = 0;
	jp->jp_headfirstlsn = 0;

	jp->jp_nextlsn = 0;

	jp->jp_jblockcount = 0;

	spinlock_init(&jp->jp_lsnmaplock);
	jp->jp_firstlsns = NULL;
	jp->jp_oldestjblock = 0;

	jp->jp_recov_tailpos.jp_jblock = 0;
	jp->jp_recov_tailpos.jp_blockoffset = 0;
	jp->jp_recov_headpos.jp_jblock = 0;
	jp->jp_recov_headpos.jp_blockoffset = 0;

	return jp;
}

void
sfs_jphys_destroy(struct sfs_jphys *jp)
{
	KASSERT(jp->jp_operatingmode == false);
	KASSERT(jp->jp_recoverymode == false);

	spinlock_cleanup(&jp->jp_lsnmaplock);
	kfree(jp->jp_firstlsns);
	KASSERT(jp->jp_headbuf == NULL);
	KASSERT(jp->jp_nextbuf == NULL);
	cv_destroy(jp->jp_nextcv);
	lock_destroy(jp->jp_lock);
	kfree(jp);
}

void
sfs_jphys_startscanning(struct sfs_jphys *jp)
{
	KASSERT(jp->jp_physrecovered);
	KASSERT(jp->jp_recoverymode == false);
	jp->jp_recoverymode = true;
}

void
sfs_jphys_stopscanning(struct sfs_jphys *jp)
{
	KASSERT(jp->jp_physrecovered);
	KASSERT(jp->jp_recoverymode);
	jp->jp_recoverymode = false;
}

int
sfs_jphys_start(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;
	uint32_t nextjblock;
	unsigned journalblocks, i;
	int result;

	KASSERT(jp->jp_physrecovered);
	KASSERT(!jp->jp_operatingmode);

	journalblocks = sfs->sfs_sb.sb_journalblocks;

	KASSERT(jp->jp_firstlsns == NULL);
	jp->jp_firstlsns = kmalloc(sizeof(sfs_lsn_t) * journalblocks);
	if (jp->jp_firstlsns == NULL) {
		return ENOMEM;
	}
	for (i=0; i<journalblocks; i++) {
		jp->jp_firstlsns[i] = 0;
	}

	/*
	 * Note: we get the journal head buffers in fsmanaged mode (see
	 * buf.h for the description) so sync operations don't try to
	 * write them out. That would deadlock (except under just the
	 * right circumstances with a busy volume) because we only
	 * release them when we've filled them. Doing this means we're
	 * responsible for making sure the buffers get written out in
	 * a timely fashion; but that will happen naturally.
	 */

	result = buffer_get_fsmanaged(&sfs->sfs_absfs,
				      sfs->sfs_sb.sb_journalstart +
				         jp->jp_headjblock,
				      SFS_BLOCKSIZE, &jp->jp_headbuf);
	if (result) {
		return result;
	}
	buffer_mark_valid(jp->jp_headbuf);

	nextjblock = jp->jp_headjblock + 1;
	if (nextjblock == journalblocks) {
		nextjblock = 0;
	}
	result = buffer_get_fsmanaged(&sfs->sfs_absfs,
				      sfs->sfs_sb.sb_journalstart + nextjblock,
				      SFS_BLOCKSIZE, &jp->jp_nextbuf);
	if (result) {
		buffer_release_and_invalidate(jp->jp_headbuf);
		return result;
	}
	buffer_mark_valid(jp->jp_nextbuf);

	jp->jp_firstlsns[jp->jp_headjblock] = jp->jp_headfirstlsn;
	jp->jp_oldestjblock = jp->jp_headjblock;

	jp->jp_operatingmode = true;
	return 0;
}

void
sfs_jphys_unstart(struct sfs_fs *sfs)
{
	struct sfs_jphys *jp = sfs->sfs_jphys;

	KASSERT(jp->jp_physrecovered);
	KASSERT(jp->jp_operatingmode);

	/*
	 * Don't assert that the journal's been flushed. If we're
	 * dying, it might not be.
	 */

	buffer_release_and_invalidate(jp->jp_headbuf);
	buffer_release_and_invalidate(jp->jp_nextbuf);

	jp->jp_headbuf = NULL;
	jp->jp_nextbuf = NULL;

	jp->jp_operatingmode = false;
}

void
sfs_jphys_stop(struct sfs_jphys *jp)
{
	lock_acquire(jp->jp_lock);

	KASSERT(jp->jp_physrecovered);
	KASSERT(jp->jp_operatingmode);

	/*
	 * We should have just checkpointed; there should not be
	 * pending journal records.
	 */
	KASSERT(jp->jp_headbyte == 0);

	/* similarly, journalheadbuf should not be dirty */
	KASSERT(!buffer_is_dirty(jp->jp_headbuf));
	buffer_release_and_invalidate(jp->jp_headbuf);
	jp->jp_headbuf = NULL;

	/* should not get here without nextbuf existing */
	KASSERT(jp->jp_nextbuf != NULL);
	KASSERT(jp->jp_gettingnext == NULL);

	/* and nextbuf should never be dirty... */
	KASSERT(!buffer_is_dirty(jp->jp_nextbuf));
	buffer_release_and_invalidate(jp->jp_nextbuf);
	jp->jp_nextbuf = NULL;

	jp->jp_operatingmode = false;
	lock_release(jp->jp_lock);
}
