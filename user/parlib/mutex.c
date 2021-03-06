/* Copyright (c) 2016-2017 Google, Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details. */

/* Generic Uthread Semaphores, Mutexes, CVs, and other synchronization
 * functions.  2LSs implement their own sync objects (bottom of the file). */

#include <parlib/uthread.h>
#include <sys/queue.h>
#include <parlib/spinlock.h>
#include <parlib/alarm.h>
#include <parlib/assert.h>
#include <malloc.h>

struct timeout_blob {
	bool						timed_out;
	struct uthread				*uth;
	uth_sync_t					*sync_ptr;
	struct spin_pdr_lock		*lock_ptr;
};

/* When sync primitives want to time out, they can use this alarm handler.  It
 * needs a timeout_blob, which is independent of any particular sync method. */
static void timeout_handler(struct alarm_waiter *waiter)
{
	struct timeout_blob *blob = (struct timeout_blob*)waiter->data;

	spin_pdr_lock(blob->lock_ptr);
	if (__uth_sync_get_uth(blob->sync_ptr, blob->uth))
		blob->timed_out = TRUE;
	spin_pdr_unlock(blob->lock_ptr);
	if (blob->timed_out)
		uthread_runnable(blob->uth);
}

/* Minor helper, sets a blob's fields */
static void set_timeout_blob(struct timeout_blob *blob, uth_sync_t *sync_ptr,
                             struct spin_pdr_lock *lock_ptr)
{
	blob->timed_out = FALSE;
	blob->uth = current_uthread;
	blob->sync_ptr = sync_ptr;
	blob->lock_ptr = lock_ptr;
}

/* Minor helper, sets an alarm for blob and a timespec */
static void set_timeout_alarm(struct alarm_waiter *waiter,
                              struct timeout_blob *blob,
                              const struct timespec *abs_timeout)
{
	init_awaiter(waiter, timeout_handler);
	waiter->data = blob;
	set_awaiter_abs_unix(waiter, timespec_to_alarm_time(abs_timeout));
	set_alarm(waiter);
}

/************** Semaphores and Mutexes **************/

static void __uth_semaphore_init(void *arg)
{
	struct uth_semaphore *sem = (struct uth_semaphore*)arg;

	spin_pdr_init(&sem->lock);
	__uth_sync_init(&sem->sync_obj);
	/* If we used a static initializer for a semaphore, count is already set.
	 * o/w it will be set by _alloc() or _init() (via uth_semaphore_init()). */
}

/* Initializes a sem acquired from somewhere else.  POSIX's sem_init() needs
 * this. */
void uth_semaphore_init(uth_semaphore_t *sem, unsigned int count)
{
	__uth_semaphore_init(sem);
	sem->count = count;
	/* The once is to make sure the object is initialized. */
	parlib_set_ran_once(&sem->once_ctl);
}

/* Undoes whatever was done in init. */
void uth_semaphore_destroy(uth_semaphore_t *sem)
{
	__uth_sync_destroy(&sem->sync_obj);
}

uth_semaphore_t *uth_semaphore_alloc(unsigned int count)
{
	struct uth_semaphore *sem;

	sem = malloc(sizeof(struct uth_semaphore));
	assert(sem);
	uth_semaphore_init(sem, count);
	return sem;
}

void uth_semaphore_free(uth_semaphore_t *sem)
{
	uth_semaphore_destroy(sem);
	free(sem);
}

static void __semaphore_cb(struct uthread *uth, void *arg)
{
	struct uth_semaphore *sem = (struct uth_semaphore*)arg;

	/* We need to tell the 2LS that its thread blocked.  We need to do this
	 * before unlocking the sem, since as soon as we unlock, the sem could be
	 * released and our thread restarted.
	 *
	 * Also note the lock-ordering rule.  The sem lock is grabbed before any
	 * locks the 2LS might grab. */
	uthread_has_blocked(uth, UTH_EXT_BLK_MUTEX);
	__uth_sync_enqueue(uth, &sem->sync_obj);
	spin_pdr_unlock(&sem->lock);
}

bool uth_semaphore_timed_down(uth_semaphore_t *sem,
                              const struct timespec *abs_timeout)
{
	struct alarm_waiter waiter[1];
	struct timeout_blob blob[1];

	assert_can_block();
	parlib_run_once(&sem->once_ctl, __uth_semaphore_init, sem);
	spin_pdr_lock(&sem->lock);
	if (sem->count > 0) {
		/* Only down if we got one.  This means a sem with no more counts is 0,
		 * not negative (where -count == nr_waiters).  Doing it this way means
		 * our timeout function works for sems and CVs. */
		sem->count--;
		spin_pdr_unlock(&sem->lock);
		return TRUE;
	}
	if (abs_timeout) {
		set_timeout_blob(blob, &sem->sync_obj, &sem->lock);
		set_timeout_alarm(waiter, blob, abs_timeout);
	}
	/* the unlock and sync enqueuing is done in the yield callback.  as always,
	 * we need to do this part in vcore context, since as soon as we unlock the
	 * uthread could restart.  (atomically yield and unlock). */
	uthread_yield(TRUE, __semaphore_cb, sem);
	if (abs_timeout) {
		/* We're guaranteed the alarm will either be cancelled or the handler
		 * complete when unset_alarm() returns. */
		unset_alarm(waiter);
		return blob->timed_out ? FALSE : TRUE;
	}
	return TRUE;
}

void uth_semaphore_down(uth_semaphore_t *sem)
{
	uth_semaphore_timed_down(sem, NULL);
}

bool uth_semaphore_trydown(uth_semaphore_t *sem)
{
	bool ret = FALSE;

	assert_can_block();
	parlib_run_once(&sem->once_ctl, __uth_semaphore_init, sem);
	spin_pdr_lock(&sem->lock);
	if (sem->count > 0) {
		sem->count--;
		ret = TRUE;
	}
	spin_pdr_unlock(&sem->lock);
	return ret;
}

void uth_semaphore_up(uth_semaphore_t *sem)
{
	struct uthread *uth;

	/* once-ing the 'up', unlike mtxs 'unlock', since sems can be special. */
	parlib_run_once(&sem->once_ctl, __uth_semaphore_init, sem);
	spin_pdr_lock(&sem->lock);
	uth = __uth_sync_get_next(&sem->sync_obj);
	/* If there was a waiter, we pass our resource/count to them. */
	if (!uth)
		sem->count++;
	spin_pdr_unlock(&sem->lock);
	if (uth)
		uthread_runnable(uth);
}

/* Takes a void * since it's called by parlib_run_once(), which enables us to
 * statically initialize the mutex.  This init does everything not done by the
 * static initializer.  Note we do not allow 'static' destruction.  (No one
 * calls free). */
static void __uth_mutex_init(void *arg)
{
	struct uth_semaphore *mtx = (struct uth_semaphore*)arg;

	__uth_semaphore_init(mtx);
	mtx->count = 1;
}

void uth_mutex_init(uth_mutex_t *mtx)
{
	__uth_mutex_init(mtx);
	parlib_set_ran_once(&mtx->once_ctl);
}

void uth_mutex_destroy(uth_mutex_t *mtx)
{
	uth_semaphore_destroy(mtx);
}

uth_mutex_t *uth_mutex_alloc(void)
{
	struct uth_semaphore *mtx;

	mtx = malloc(sizeof(struct uth_semaphore));
	assert(mtx);
	uth_mutex_init(mtx);
	return mtx;
}

void uth_mutex_free(uth_mutex_t *mtx)
{
	uth_semaphore_free(mtx);
}

bool uth_mutex_timed_lock(uth_mutex_t *mtx, const struct timespec *abs_timeout)
{
	parlib_run_once(&mtx->once_ctl, __uth_mutex_init, mtx);
	return uth_semaphore_timed_down(mtx, abs_timeout);
}

void uth_mutex_lock(uth_mutex_t *mtx)
{
	parlib_run_once(&mtx->once_ctl, __uth_mutex_init, mtx);
	uth_semaphore_down(mtx);
}

bool uth_mutex_trylock(uth_mutex_t *mtx)
{
	parlib_run_once(&mtx->once_ctl, __uth_mutex_init, mtx);
	return uth_semaphore_trydown(mtx);
}

void uth_mutex_unlock(uth_mutex_t *mtx)
{
	uth_semaphore_up(mtx);
}

/************** Recursive mutexes **************/

static void __uth_recurse_mutex_init(void *arg)
{
	struct uth_recurse_mutex *r_mtx = (struct uth_recurse_mutex*)arg;

	__uth_mutex_init(&r_mtx->mtx);
	/* Since we always manually call __uth_mutex_init(), there's no reason to
	 * mess with the regular mutex's static initializer.  Just say it's been
	 * done. */
	parlib_set_ran_once(&r_mtx->mtx.once_ctl);
	r_mtx->lockholder = NULL;
	r_mtx->count = 0;
}

void uth_recurse_mutex_init(uth_recurse_mutex_t *r_mtx)
{
	__uth_recurse_mutex_init(r_mtx);
	parlib_set_ran_once(&r_mtx->once_ctl);
}

void uth_recurse_mutex_destroy(uth_recurse_mutex_t *r_mtx)
{
	uth_semaphore_destroy(&r_mtx->mtx);
}

uth_recurse_mutex_t *uth_recurse_mutex_alloc(void)
{
	struct uth_recurse_mutex *r_mtx = malloc(sizeof(struct uth_recurse_mutex));

	assert(r_mtx);
	uth_recurse_mutex_init(r_mtx);
	return r_mtx;
}

void uth_recurse_mutex_free(uth_recurse_mutex_t *r_mtx)
{
	uth_recurse_mutex_destroy(r_mtx);
	free(r_mtx);
}

bool uth_recurse_mutex_timed_lock(uth_recurse_mutex_t *r_mtx,
                                  const struct timespec *abs_timeout)
{
	assert_can_block();
	parlib_run_once(&r_mtx->once_ctl, __uth_recurse_mutex_init, r_mtx);
	/* We don't have to worry about races on current_uthread or count.  They are
	 * only written by the initial lockholder, and this check will only be true
	 * for the initial lockholder, which cannot concurrently call this function
	 * twice (a thread is single-threaded).
	 *
	 * A signal handler running for a thread should not attempt to grab a
	 * recursive mutex (that's probably a bug).  If we need to support that,
	 * we'll have to disable notifs temporarily. */
	if (r_mtx->lockholder == current_uthread) {
		r_mtx->count++;
		return TRUE;
	}
	if (!uth_mutex_timed_lock(&r_mtx->mtx, abs_timeout))
		return FALSE;
	r_mtx->lockholder = current_uthread;
	r_mtx->count = 1;
	return TRUE;
}

void uth_recurse_mutex_lock(uth_recurse_mutex_t *r_mtx)
{
	uth_recurse_mutex_timed_lock(r_mtx, NULL);
}

bool uth_recurse_mutex_trylock(uth_recurse_mutex_t *r_mtx)
{
	bool ret;

	assert_can_block();
	parlib_run_once(&r_mtx->once_ctl, __uth_recurse_mutex_init, r_mtx);
	if (r_mtx->lockholder == current_uthread) {
		r_mtx->count++;
		return TRUE;
	}
	ret = uth_mutex_trylock(&r_mtx->mtx);
	if (ret) {
		r_mtx->lockholder = current_uthread;
		r_mtx->count = 1;
	}
	return ret;
}

void uth_recurse_mutex_unlock(uth_recurse_mutex_t *r_mtx)
{
	r_mtx->count--;
	if (!r_mtx->count) {
		r_mtx->lockholder = NULL;
		uth_mutex_unlock(&r_mtx->mtx);
	}
}


/************** Condition Variables **************/


static void __uth_cond_var_init(void *arg)
{
	struct uth_cond_var *cv = (struct uth_cond_var*)arg;

	spin_pdr_init(&cv->lock);
	__uth_sync_init(&cv->sync_obj);
}

void uth_cond_var_init(uth_cond_var_t *cv)
{
	__uth_cond_var_init(cv);
	parlib_set_ran_once(&cv->once_ctl);
}

void uth_cond_var_destroy(uth_cond_var_t *cv)
{
	__uth_sync_destroy(&cv->sync_obj);
}

uth_cond_var_t *uth_cond_var_alloc(void)
{
	struct uth_cond_var *cv;

	cv = malloc(sizeof(struct uth_cond_var));
	assert(cv);
	uth_cond_var_init(cv);
	return cv;
}

void uth_cond_var_free(uth_cond_var_t *cv)
{
	uth_cond_var_destroy(cv);
	free(cv);
}

struct uth_cv_link {
	struct uth_cond_var			*cv;
	struct uth_semaphore		*mtx;
};

static void __cv_wait_cb(struct uthread *uth, void *arg)
{
	struct uth_cv_link *link = (struct uth_cv_link*)arg;
	struct uth_cond_var *cv = link->cv;
	struct uth_semaphore *mtx = link->mtx;

	/* We need to tell the 2LS that its thread blocked.  We need to do this
	 * before unlocking the cv, since as soon as we unlock, the cv could be
	 * signalled and our thread restarted.
	 *
	 * Also note the lock-ordering rule.  The cv lock is grabbed before any
	 * locks the 2LS might grab. */
	uthread_has_blocked(uth, UTH_EXT_BLK_MUTEX);
	__uth_sync_enqueue(uth, &cv->sync_obj);
	spin_pdr_unlock(&cv->lock);
	/* This looks dangerous, since both the CV and MTX could use the
	 * uth->sync_next TAILQ_ENTRY (or whatever the 2LS uses), but the uthread
	 * never sleeps on both at the same time.  We *hold* the mtx - we aren't
	 * *sleeping* on it.  Sleeping uses the sync_next.  Holding it doesn't.
	 *
	 * Next, consider what happens as soon as we unlock the CV.  Our thread
	 * could get woken up, and then immediately try to grab the mtx and go to
	 * sleep! (see below).  If that happens, the uthread is no longer sleeping
	 * on the CV, and the sync_next is free.  The invariant is that a uthread
	 * can only sleep on one sync_object at a time. */
	uth_mutex_unlock(mtx);
}

/* Caller holds mtx.  We will 'atomically' release it and wait.  On return,
 * caller holds mtx again.  Once our uth is on the CV's list, we can release the
 * mtx without fear of missing a signal.
 *
 * POSIX refers to atomicity in this context as "atomically with respect to
 * access by another thread to the mutex and then the condition variable"
 *
 * The idea is that we hold the mutex to protect some invariant; we check it,
 * and decide to sleep.  Now we get on the list before releasing so that any
 * changes to that invariant (e.g. a flag is now TRUE) happen after we're on the
 * list, and so that we don't miss the signal.  To be more clear, the invariant
 * in a basic wake-up flag scenario is: "whenever a flag is set from FALSE to
 * TRUE, all waiters that saw FALSE are on the CV's waitqueue."  The mutex is
 * required for this invariant.
 *
 * Note that signal/broadcasters do not *need* to hold the mutex, in general,
 * but they do in the basic wake-up flag scenario.  If not, the race is this:
 *
 * Sleeper:								Waker:
 * -----------------------------------------------------------------
 * Hold mutex
 *   See flag is False
 *   Decide to sleep
 *										Set flag True
 * PAUSE!								Grab CV lock
 *										See list is empty, unlock
 *
 *   Grab CV lock
 *     Get put on list
 *   Unlock CV lock
 * Unlock mutex
 * (Never wake up; we missed the signal)
 *
 * For those familiar with the kernel's CVs, we don't couple mutexes with CVs.
 * cv_lock() actually grabs the spinlock inside the CV and uses *that* to
 * protect the invariant.  The signallers always grab that lock, so the sleeper
 * is not in danger of missing the signal.  The tradeoff is that the kernel CVs
 * use a spinlock instead of a mutex for protecting its invariant; there might
 * be some case that preferred blocking sync.
 *
 * The uthread CVs take a mutex, unlike the kernel CVs, to map more cleanly to
 * POSIX CVs.  Maybe one approach or the other is a bad idea; we'll see.
 *
 * As far as lock ordering goes, once the sleeper holds the mutex and is on the
 * CV's list, it can unlock in any order it wants.  However, unlocking a mutex
 * actually requires grabbing its spinlock.  So as to not have a lock ordering
 * between *spinlocks*, we let go of the CV's spinlock before unlocking the
 * mutex.  There is an ordering between the mutex and the CV spinlock (mutex->cv
 * spin), but there is no ordering between the mutex spin and cv spin.  And of
 * course, we need to unlock the CV spinlock in the yield callback.
 *
 * Also note that we use the external API for the mutex operations.  A 2LS could
 * have their own mutex ops but still use the generic cv ops. */
bool uth_cond_var_timed_wait(uth_cond_var_t *cv, uth_mutex_t *mtx,
                             const struct timespec *abs_timeout)
{
	struct uth_cv_link link;
	struct alarm_waiter waiter[1];
	struct timeout_blob blob[1];
	bool ret = TRUE;

	assert_can_block();
	parlib_run_once(&cv->once_ctl, __uth_cond_var_init, cv);
	link.cv = cv;
	link.mtx = mtx;
	spin_pdr_lock(&cv->lock);
	if (abs_timeout) {
		set_timeout_blob(blob, &cv->sync_obj, &cv->lock);
		set_timeout_alarm(waiter, blob, abs_timeout);
	}
	uthread_yield(TRUE, __cv_wait_cb, &link);
	if (abs_timeout) {
		unset_alarm(waiter);
		ret = blob->timed_out ? FALSE : TRUE;
	}
	uth_mutex_lock(mtx);
	return ret;
}

void uth_cond_var_wait(uth_cond_var_t *cv, uth_mutex_t *mtx)
{
	uth_cond_var_timed_wait(cv, mtx, NULL);
}

/* GCC doesn't list this as one of the C++0x functions, but it's easy to do and
 * implement uth_cond_var_wait_recurse() with it, just like for all the other
 * 'timed' functions.
 *
 * Note the timeout applies to getting the signal on the CV, not on reacquiring
 * the mutex. */
bool uth_cond_var_timed_wait_recurse(uth_cond_var_t *cv,
                                     uth_recurse_mutex_t *r_mtx,
                                     const struct timespec *abs_timeout)
{
	unsigned int old_count = r_mtx->count;
	bool ret;

	/* In cond_wait, we're going to unlock the internal mutex.  We'll do the
	 * prep-work for that now.  (invariant is that an unlocked r_mtx has no
	 * lockholder and count == 0. */
	r_mtx->lockholder = NULL;
	r_mtx->count = 0;
	ret = uth_cond_var_timed_wait(cv, &r_mtx->mtx, abs_timeout);
	/* Now we hold the internal mutex again.  Need to restore the tracking. */
	r_mtx->lockholder = current_uthread;
	r_mtx->count = old_count;
	return ret;
}

/* GCC wants this function, though its semantics are a little unclear.  I
 * imagine you'd want to completely unlock it (say you locked it 3 times), and
 * when you get it back, that you have your three locks back. */
void uth_cond_var_wait_recurse(uth_cond_var_t *cv, uth_recurse_mutex_t *r_mtx)
{
	uth_cond_var_timed_wait_recurse(cv, r_mtx, NULL);
}

void uth_cond_var_signal(uth_cond_var_t *cv)
{
	struct uthread *uth;

	parlib_run_once(&cv->once_ctl, __uth_cond_var_init, cv);
	spin_pdr_lock(&cv->lock);
	uth = __uth_sync_get_next(&cv->sync_obj);
	spin_pdr_unlock(&cv->lock);
	if (uth)
		uthread_runnable(uth);
}

void uth_cond_var_broadcast(uth_cond_var_t *cv)
{
	uth_sync_t restartees;

	parlib_run_once(&cv->once_ctl, __uth_cond_var_init, cv);
	spin_pdr_lock(&cv->lock);
	if (__uth_sync_is_empty(&cv->sync_obj)) {
		spin_pdr_unlock(&cv->lock);
		return;
	}
	__uth_sync_init(&restartees);
	__uth_sync_swap(&restartees, &cv->sync_obj);
	spin_pdr_unlock(&cv->lock);
	__uth_sync_wake_all(&restartees);
}


/************** Reader-writer Sleeping Locks **************/


static void __uth_rwlock_init(void *arg)
{
	struct uth_rwlock *rwl = (struct uth_rwlock*)arg;

	spin_pdr_init(&rwl->lock);
	rwl->nr_readers = 0;
	rwl->has_writer = FALSE;
	__uth_sync_init(&rwl->readers);
	__uth_sync_init(&rwl->writers);
}

void uth_rwlock_init(uth_rwlock_t *rwl)
{
	__uth_rwlock_init(rwl);
	parlib_set_ran_once(&rwl->once_ctl);
}

void uth_rwlock_destroy(uth_rwlock_t *rwl)
{
	__uth_sync_destroy(&rwl->readers);
	__uth_sync_destroy(&rwl->writers);
}

uth_rwlock_t *uth_rwlock_alloc(void)
{
	struct uth_rwlock *rwl;

	rwl = malloc(sizeof(struct uth_rwlock));
	assert(rwl);
	uth_rwlock_init(rwl);
	return rwl;
}

void uth_rwlock_free(uth_rwlock_t *rwl)
{
	uth_rwlock_destroy(rwl);
	free(rwl);
}

/* Readers and writers block until they have the lock.  The delicacies are dealt
 * with by the unlocker. */
static void __rwlock_rd_cb(struct uthread *uth, void *arg)
{
	struct uth_rwlock *rwl = (struct uth_rwlock*)arg;

	uthread_has_blocked(uth, UTH_EXT_BLK_MUTEX);
	__uth_sync_enqueue(uth, &rwl->readers);
	spin_pdr_unlock(&rwl->lock);
}

void uth_rwlock_rdlock(uth_rwlock_t *rwl)
{
	assert_can_block();
	parlib_run_once(&rwl->once_ctl, __uth_rwlock_init, rwl);
	spin_pdr_lock(&rwl->lock);
	/* Readers always make progress when there is no writer */
	if (!rwl->has_writer) {
		rwl->nr_readers++;
		spin_pdr_unlock(&rwl->lock);
		return;
	}
	uthread_yield(TRUE, __rwlock_rd_cb, rwl);
}

bool uth_rwlock_try_rdlock(uth_rwlock_t *rwl)
{
	bool ret = FALSE;

	assert_can_block();
	parlib_run_once(&rwl->once_ctl, __uth_rwlock_init, rwl);
	spin_pdr_lock(&rwl->lock);
	if (!rwl->has_writer) {
		rwl->nr_readers++;
		ret = TRUE;
	}
	spin_pdr_unlock(&rwl->lock);
	return ret;
}

static void __rwlock_wr_cb(struct uthread *uth, void *arg)
{
	struct uth_rwlock *rwl = (struct uth_rwlock*)arg;

	uthread_has_blocked(uth, UTH_EXT_BLK_MUTEX);
	__uth_sync_enqueue(uth, &rwl->writers);
	spin_pdr_unlock(&rwl->lock);
}

void uth_rwlock_wrlock(uth_rwlock_t *rwl)
{
	assert_can_block();
	parlib_run_once(&rwl->once_ctl, __uth_rwlock_init, rwl);
	spin_pdr_lock(&rwl->lock);
	/* Writers require total mutual exclusion - no writers or readers */
	if (!rwl->has_writer && !rwl->nr_readers) {
		rwl->has_writer = TRUE;
		spin_pdr_unlock(&rwl->lock);
		return;
	}
	uthread_yield(TRUE, __rwlock_wr_cb, rwl);
}

bool uth_rwlock_try_wrlock(uth_rwlock_t *rwl)
{
	bool ret = FALSE;

	assert_can_block();
	parlib_run_once(&rwl->once_ctl, __uth_rwlock_init, rwl);
	spin_pdr_lock(&rwl->lock);
	if (!rwl->has_writer && !rwl->nr_readers) {
		rwl->has_writer = TRUE;
		ret = TRUE;
	}
	spin_pdr_unlock(&rwl->lock);
	return ret;
}

/* Let's try to wake writers (yes, this is a policy decision), and if none, wake
 * all the readers.  The invariant there is that if there is no writer, then
 * there are no waiting readers. */
static void __rw_unlock_writer(struct uth_rwlock *rwl,
                               struct uth_tailq *restartees)
{
	struct uthread *uth;

	uth = __uth_sync_get_next(&rwl->writers);
	if (uth) {
		TAILQ_INSERT_TAIL(restartees, uth, sync_next);
	} else {
		rwl->has_writer = FALSE;
		while ((uth = __uth_sync_get_next(&rwl->readers))) {
			TAILQ_INSERT_TAIL(restartees, uth, sync_next);
			rwl->nr_readers++;
		}
	}
}

static void __rw_unlock_reader(struct uth_rwlock *rwl,
                               struct uth_tailq *restartees)
{
	struct uthread *uth;

	rwl->nr_readers--;
	if (!rwl->nr_readers) {
		uth = __uth_sync_get_next(&rwl->writers);
		if (uth) {
			TAILQ_INSERT_TAIL(restartees, uth, sync_next);
			rwl->has_writer = TRUE;
		}
	}
}

/* Unlock works for either readers or writer locks.  You can tell which you were
 * based on whether has_writer is set or not. */
void uth_rwlock_unlock(uth_rwlock_t *rwl)
{
	struct uth_tailq restartees = TAILQ_HEAD_INITIALIZER(restartees);
	struct uthread *i, *safe;

	spin_pdr_lock(&rwl->lock);
	if (rwl->has_writer)
		__rw_unlock_writer(rwl, &restartees);
	else
		__rw_unlock_reader(rwl, &restartees);
	spin_pdr_unlock(&rwl->lock);
	TAILQ_FOREACH_SAFE(i, &restartees, sync_next, safe)
		uthread_runnable(i);
}


/************** Default Sync Obj Implementation **************/

static void uth_default_sync_init(uth_sync_t *sync)
{
	struct uth_tailq *tq = (struct uth_tailq*)sync;

	parlib_static_assert(sizeof(struct uth_tailq) <= sizeof(uth_sync_t));
	TAILQ_INIT(tq);
}

static void uth_default_sync_destroy(uth_sync_t *sync)
{
	struct uth_tailq *tq = (struct uth_tailq*)sync;

	assert(TAILQ_EMPTY(tq));
}

static void uth_default_sync_enqueue(struct uthread *uth, uth_sync_t *sync)
{
	struct uth_tailq *tq = (struct uth_tailq*)sync;

	TAILQ_INSERT_TAIL(tq, uth, sync_next);
}

static struct uthread *uth_default_sync_get_next(uth_sync_t *sync)
{
	struct uth_tailq *tq = (struct uth_tailq*)sync;
	struct uthread *first;

	first = TAILQ_FIRST(tq);
	if (first)
		TAILQ_REMOVE(tq, first, sync_next);
	return first;
}

static bool uth_default_sync_get_uth(uth_sync_t *sync, struct uthread *uth)
{
	struct uth_tailq *tq = (struct uth_tailq*)sync;
	struct uthread *i;

	TAILQ_FOREACH(i, tq, sync_next) {
		if (i == uth) {
			TAILQ_REMOVE(tq, i, sync_next);
			return TRUE;
		}
	}
	return FALSE;
}

static void uth_default_sync_swap(uth_sync_t *a, uth_sync_t *b)
{
	struct uth_tailq *tq_a = (struct uth_tailq*)a;
	struct uth_tailq *tq_b = (struct uth_tailq*)b;

	TAILQ_SWAP(tq_a, tq_b, uthread, sync_next);
}

static bool uth_default_sync_is_empty(uth_sync_t *sync)
{
	struct uth_tailq *tq = (struct uth_tailq*)sync;

	return TAILQ_EMPTY(tq);
}

/************** External uthread sync interface **************/

/* Called by 2LS-independent sync code when a sync object needs initialized. */
void __uth_sync_init(uth_sync_t *sync)
{
	if (sched_ops->sync_init) {
		sched_ops->sync_init(sync);
		return;
	}
	uth_default_sync_init(sync);
}

/* Called by 2LS-independent sync code when a sync object is destroyed. */
void __uth_sync_destroy(uth_sync_t *sync)
{
	if (sched_ops->sync_destroy) {
		sched_ops->sync_destroy(sync);
		return;
	}
	uth_default_sync_destroy(sync);
}

/* Called by 2LS-independent sync code when a thread blocks on sync */
void __uth_sync_enqueue(struct uthread *uth, uth_sync_t *sync)
{
	if (sched_ops->sync_enqueue) {
		sched_ops->sync_enqueue(uth, sync);
		return;
	}
	uth_default_sync_enqueue(uth, sync);
}

/* Called by 2LS-independent sync code when a thread needs to be woken. */
struct uthread *__uth_sync_get_next(uth_sync_t *sync)
{
	if (sched_ops->sync_get_next)
		return sched_ops->sync_get_next(sync);
	return uth_default_sync_get_next(sync);
}

/* Called by 2LS-independent sync code when a specific thread needs to be woken.
 * Returns TRUE if the uthread was blocked on the object, FALSE o/w. */
bool __uth_sync_get_uth(uth_sync_t *sync, struct uthread *uth)
{
	if (sched_ops->sync_get_uth)
		return sched_ops->sync_get_uth(sync, uth);
	return uth_default_sync_get_uth(sync, uth);
}

/* Called by 2LS-independent sync code to swap members of sync objects. */
void __uth_sync_swap(uth_sync_t *a, uth_sync_t *b)
{
	if (sched_ops->sync_swap) {
		sched_ops->sync_swap(a, b);
		return;
	}
	uth_default_sync_swap(a, b);
}

/* Called by 2LS-independent sync code */
bool __uth_sync_is_empty(uth_sync_t *sync)
{
	if (sched_ops->sync_is_empty)
		return sched_ops->sync_is_empty(sync);
	return uth_default_sync_is_empty(sync);
}

/* Called by 2LS-independent sync code to wake up all uths on sync.  You should
 * probably not hold locks while you do this - swap the items to a local sync
 * object first. */
void __uth_sync_wake_all(uth_sync_t *wakees)
{
	struct uthread *uth_i;

	if (sched_ops->thread_bulk_runnable) {
		sched_ops->thread_bulk_runnable(wakees);
	} else {
		while ((uth_i = __uth_sync_get_next(wakees)))
			uthread_runnable(uth_i);
	}
}
