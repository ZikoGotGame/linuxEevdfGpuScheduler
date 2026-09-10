// SPDX-License-Identifier: MIT
/* Copyright 2015 Advanced Micro Devices, Inc. */
/* Copyright (c) 2025 Valve Corporation */

#include "linux/completion.h"
#include "linux/ktime.h"
#include "linux/math64.h"
#include "linux/spinlock.h"
#include "linux/types.h"
#include <linux/rbtree.h>

#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>

#include "sched_internal.h"

static __always_inline bool
drm_sched_entity_compare_before(struct rb_node *a, const struct rb_node *b)
{
	struct drm_sched_entity *ea =
		rb_entry((a), struct drm_sched_entity, rb_tree_node);
	struct drm_sched_entity *eb =
		rb_entry((b), struct drm_sched_entity, rb_tree_node);

	return ktime_before(ea->oldest_job_waiting, eb->oldest_job_waiting);
}

/* MODIFIED: added by Zac */
const s64 drm_sched_prio_weight[] = {
	[DRM_SCHED_PRIORITY_KERNEL] = 64,
	[DRM_SCHED_PRIORITY_HIGH] = 32,
	[DRM_SCHED_PRIORITY_NORMAL] = 8,
	[DRM_SCHED_PRIORITY_LOW] = 1,
};

/*
* MODIFIED: added by Zac Tawfick
*
* Add the vruntime and weight of an entity to the average weighted vruntime
* of the run queue.
*/
void drm_sched_rq_avg_add(struct drm_sched_rq *rq,
			  struct drm_sched_entity *entity)
{
	s64 entity_weight = drm_sched_prio_weight[entity->priority];
	s64 w_vruntime;

	w_vruntime = ktime_to_ns(entity->vruntime) * entity_weight;

	lockdep_assert_held(&rq->lock);
	WARN_ON_ONCE((w_vruntime >> 63) != (w_vruntime >> 62));

	rq->vruntime_sum += w_vruntime;
	rq->weight_sum += entity_weight;
}

/*
* MODIFIED: added by Zac Tawfick
*
* Subtract the vruntime and weight of an entity from the average weighted vruntime
* of the run queue.
*/
void drm_sched_rq_avg_sub(struct drm_sched_rq *rq,
			  struct drm_sched_entity *entity)
{
	lockdep_assert_held(&rq->lock);

	s64 entity_weight = drm_sched_prio_weight[entity->priority];
	s64 w_vruntime;

	w_vruntime = ktime_to_ns(entity->vruntime) * entity_weight;

	rq->vruntime_sum -= w_vruntime;
	rq->weight_sum -= entity_weight;

	if (!rq->weight_sum)
		WARN_ON_ONCE(rq->vruntime_sum);
}

/*
* MODIFIED: added by Zac Tawfick
*
* Get the average weighted virtual runtime of a run queue.
*/
ktime_t drm_sched_rq_avg_vruntime(struct drm_sched_rq *rq)
{
	lockdep_assert_held(&rq->lock);

	if (rq->weight_sum)
		rq->last_vruntime = ns_to_ktime(
			div64_s64(rq->vruntime_sum, rq->weight_sum));

	return rq->last_vruntime;
}

static void drm_sched_rq_update_prio(struct drm_sched_rq *rq)
{
	enum drm_sched_priority prio = DRM_SCHED_PRIORITY_INVALID;
	struct rb_node *rb;

	lockdep_assert_held(&rq->lock);

	rb = rb_first_cached(&rq->rb_tree_root);
	if (rb) {
		struct drm_sched_entity *entity =
			rb_entry(rb, typeof(*entity), rb_tree_node);

		/*
		 * The normal locking order is entity then run-queue so taking
		 * the entity lock here would be a locking inversion for the
		 * case when the current head of the run-queue is different from
		 * the one we already have locked. The unlocked read is fine
		 * though, because if the priority had just changed it is no big
		 * deal for our algorithm, but just a transient reachable only
		 * by drivers with userspace dynamic priority changes API. Equal
		 * in effect to the priority change becoming visible a few
		 * instructions later.
		 */
		prio = READ_ONCE(entity->priority);
	}

	rq->head_prio = prio;
}

void drm_sched_rq_remove_fifo_locked(struct drm_sched_entity *entity,
				     struct drm_sched_rq *rq)
{
	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	if (!RB_EMPTY_NODE(&entity->rb_tree_node)) {
		/* MODIFIED: added by Zac Tawfick */
		drm_sched_rq_avg_sub(rq, entity);
		rb_erase_cached(&entity->rb_tree_node, &rq->rb_tree_root);
		RB_CLEAR_NODE(&entity->rb_tree_node);
		drm_sched_rq_update_prio(rq);
	}
}

void drm_sched_rq_update_fifo_locked(struct drm_sched_entity *entity,
				     struct drm_sched_rq *rq, ktime_t ts)
{
	/*
	 * Both locks need to be grabbed, one to protect from entity->rq change
	 * for entity from within concurrent drm_sched_entity_select_rq and the
	 * other to update the rb tree structure.
	 */
	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	drm_sched_rq_remove_fifo_locked(entity, rq);

	entity->oldest_job_waiting = ts;

	/* MODIFIED: added by Zac Tawfick */
	drm_sched_rq_avg_add(rq, entity);
	rb_add_cached(&entity->rb_tree_node, &rq->rb_tree_root,
		      drm_sched_entity_compare_before);
	drm_sched_rq_update_prio(rq);
}

/**
 * drm_sched_rq_init - initialize a given run queue struct
 * @sched: scheduler instance to associate with this run queue
 * @rq: scheduler run queue
 *
 * Initializes a scheduler runqueue.
 */
void drm_sched_rq_init(struct drm_gpu_scheduler *sched, struct drm_sched_rq *rq)
{
	spin_lock_init(&rq->lock);
	INIT_LIST_HEAD(&rq->entities);
	rq->rb_tree_root = RB_ROOT_CACHED;
	rq->sched = sched;
	rq->head_prio = DRM_SCHED_PRIORITY_INVALID;
	rq->vruntime_sum = 0;
	rq->weight_sum = 0;
	rq->last_vruntime = 0;
}

/*
 * Core part of the CFS-like algorithm is that the virtual runtime of lower
 * priority tasks should grow quicker than the higher priority ones, so that
 * when we then schedule entities with the aim of keeping their accumulated
 * virtual time balanced, we can approach fair distribution of GPU time.
 *
 * For converting the real GPU time into virtual we pick some multipliers with
 * the idea to achieve the following GPU time distribution:
 *
 *  - Kernel priority gets roughly 2x GPU time compared to high.
 *  - High gets ~4x relative to normal.
 *  - Normal gets ~8x relative to low.
 */
static const unsigned int vruntime_shift[] = {
	[DRM_SCHED_PRIORITY_KERNEL] = 1,
	[DRM_SCHED_PRIORITY_HIGH] = 2,
	[DRM_SCHED_PRIORITY_NORMAL] = 4,
	[DRM_SCHED_PRIORITY_LOW] = 7,
};

static ktime_t drm_sched_rq_get_min_vruntime(struct drm_sched_rq *rq)
{
	ktime_t vruntime = 0;
	struct rb_node *rb;

	lockdep_assert_held(&rq->lock);

	rb = rb_first_cached(&rq->rb_tree_root);
	if (rb) {
		struct drm_sched_entity *entity =
			rb_entry(rb, typeof(*entity), rb_tree_node);
		struct drm_sched_entity_stats *stats = entity->stats;

		spin_lock(&stats->lock);
		vruntime = stats->vruntime;
		spin_unlock(&stats->lock);
	}

	return vruntime;
}

static void drm_sched_entity_save_vruntime(struct drm_sched_entity *entity,
					   ktime_t min_vruntime)
{
	struct drm_sched_entity_stats *stats = entity->stats;
	ktime_t vruntime;

	spin_lock(&stats->lock);
	vruntime = stats->vruntime;
	if (min_vruntime && vruntime > min_vruntime)
		vruntime = ktime_sub(vruntime, min_vruntime);
	else
		vruntime = 0;
	stats->vruntime = vruntime;
	spin_unlock(&stats->lock);
}

static ktime_t
drm_sched_entity_restore_vruntime(struct drm_sched_entity *entity,
				  ktime_t min_vruntime,
				  enum drm_sched_priority rq_prio)
{
	struct drm_sched_entity_stats *stats = entity->stats;
	struct drm_gpu_scheduler *sched = entity->rq->sched;
	enum drm_sched_priority prio = entity->priority;
	unsigned long avg_us, sched_avg_us;
	ktime_t vruntime;

	BUILD_BUG_ON(DRM_SCHED_PRIORITY_NORMAL < DRM_SCHED_PRIORITY_HIGH);

	spin_lock(&stats->lock);
	vruntime = stats->vruntime;
	avg_us = ewma_drm_sched_avgtime_read(&stats->avg_job_us);
	/*
	 * Unlocked read of the scheduler average is fine since it is just
	 * heuristics and data type is a natural word size.
	 */
	sched_avg_us = ewma_drm_sched_avgtime_read(&sched->avg_job_us);

	/*
	 * Special handling for entities which were picked from the top of the
	 * queue and are now re-joining the top with another one already there.
	 */
	if (!vruntime && rq_prio != DRM_SCHED_PRIORITY_INVALID) {
		if (prio > rq_prio) {
			/*
			 * Lower priority should not overtake higher when re-
			 * joining at the top of the queue so push it back
			 * somewhere behind the "middle" of the run-queue,
			 * proportional to the scheduler and entity average job
			 * durations.
			 */
			vruntime = us_to_ktime((1 + avg_us + sched_avg_us)
					       << vruntime_shift[prio]);
		} else if (prio < rq_prio) {
			/*
			 * Higher priority can go first.
			 */
			vruntime = -ns_to_ktime(rq_prio - prio);
		} else {
			/* Favour entity with shorter jobs (interactivity). */
			if (avg_us <= sched_avg_us)
				vruntime = -ns_to_ktime(1);
			else
				vruntime = ns_to_ktime(1);
		}
	}

	/*
	 * Restore saved relative position in the queue.
	 */
	vruntime = ktime_add(min_vruntime, vruntime);

	stats->vruntime = vruntime;
	spin_unlock(&stats->lock);

	return vruntime;
}

/**
* MODIFIED: added by Zac Tawfick
*
* Save the lag for an entity when it leaves the run queue.
*/
static void drm_sched_entity_save_lag(struct drm_sched_entity *entity,
				      ktime_t avg)
{
	struct drm_sched_entity_stats *stats = entity->stats;
	s64 lag;

	lockdep_assert_held(&entity->rq->lock);

	spin_lock(&stats->lock);
	lag = ktime_to_ns(ktime_sub(avg, stats->vruntime));
	spin_unlock(&stats->lock);

	entity->lag =
		clamp_t(s64, lag, -DRM_SCHED_LAG_MAX_NS, DRM_SCHED_LAG_MAX_NS);
}

/**
* MODIFIED: added by Zac Tawfick
*
* Restore entity virtual runtime from lag when joining a run queue.
*/
static ktime_t drm_sched_entity_restore_lag(struct drm_sched_rq *rq,
					    struct drm_sched_entity *entity)
{
	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	struct drm_sched_entity_stats *stats = entity->stats;
	ktime_t avg = drm_sched_rq_avg_vruntime(rq);
	ktime_t vruntime;
	s64 lag = entity->lag;

	if (!rq->weight_sum)
		lag = 0;

	vruntime = ktime_sub(avg, ns_to_ktime(lag));

	spin_lock(&stats->lock);
	stats->vruntime = vruntime;
	spin_unlock(&stats->lock);

	entity->vruntime = vruntime;
	entity->lag = 0;

	return vruntime;
}

static ktime_t drm_sched_entity_update_vruntime(struct drm_sched_entity *entity)
{
	struct drm_sched_entity_stats *stats = entity->stats;
	ktime_t runtime, prev;

	spin_lock(&stats->lock);
	prev = stats->prev_runtime;
	runtime = stats->runtime;
	stats->prev_runtime = runtime;
	runtime = ktime_add_ns(stats->vruntime,
			       ktime_to_ns(ktime_sub(runtime, prev))
				       << vruntime_shift[entity->priority]);
	stats->vruntime = runtime;
	spin_unlock(&stats->lock);

	return runtime;
}

/*
* MODIFIED: added by Zac Tawfick
*
* calculate the virtual deadline for an entity.
*/
ktime_t drm_sched_entity_calc_vdeadline(struct drm_sched_entity *entity)
{
	lockdep_assert_held(&entity->rq->lock);

	struct drm_sched_entity_stats *stats = entity->stats;
	struct drm_sched_rq *rq = entity->rq;
	struct drm_gpu_scheduler *sched = rq->sched;
	s64 entity_weight = drm_sched_prio_weight[entity->priority];
	s64 weight_ref = drm_sched_prio_weight[DRM_SCHED_PRIORITY_NORMAL];
	s64 slice, vslice;
	ktime_t vruntime, runtime, prev;
	bool counted = !RB_EMPTY_NODE(&entity->rb_tree_node);

	if (counted)
		drm_sched_rq_avg_sub(rq, entity);

	spin_lock(&stats->lock);
	slice = ewma_drm_sched_avgtime_read(&stats->avg_job_us);
	prev = stats->prev_runtime;
	runtime = stats->runtime;
	stats->prev_runtime = runtime;
	stats->vruntime = ktime_add_ns(
		stats->vruntime, ktime_to_ns(ktime_sub(runtime, prev)) *
					 div64_s64(weight_ref, entity_weight));
	vruntime = stats->vruntime;
	spin_unlock(&stats->lock);

	if (!slice)
		slice = ewma_drm_sched_avgtime_read(&sched->avg_job_us);
	if (!slice)
		slice = DRM_SCHED_SLICE_DEFAULT_US;

	slice = clamp_t(s64, slice, DRM_SCHED_SLICE_MIN_US,
			DRM_SCHED_SLICE_MAX_US);

	vslice = slice * NSEC_PER_USEC * div64_s64(weight_ref, entity_weight);
	entity->vruntime = vruntime;

	if (counted)
		drm_sched_rq_avg_add(rq, entity);

	return vruntime + vslice;
}

static ktime_t drm_sched_entity_get_job_ts(struct drm_sched_entity *entity)
{
	return drm_sched_entity_update_vruntime(entity);
}

/**
 * drm_sched_rq_add_entity - add an entity
 * @entity: scheduler entity
 * @ts: submission timestamp
 *
 * Adds a scheduler entity to the run queue.
 *
 * Return: DRM scheduler selected to handle this entity or NULL if entity has
 * been stopped and cannot be submitted to.
 */
struct drm_gpu_scheduler *
drm_sched_rq_add_entity(struct drm_sched_entity *entity, ktime_t ts)
{
	struct drm_gpu_scheduler *sched;
	struct drm_sched_rq *rq;

	/* Add the entity to the run queue */
	spin_lock(&entity->lock);
	if (entity->stopped) {
		spin_unlock(&entity->lock);

		DRM_ERROR("Trying to push to a killed entity\n");
		return NULL;
	}

	rq = entity->rq;
	spin_lock(&rq->lock);
	sched = rq->sched;

	if (list_empty(&entity->list)) {
		atomic_inc(sched->score);
		list_add_tail(&entity->list, &rq->entities);
	}

	if (drm_sched_policy == DRM_SCHED_POLICY_FAIR) {
		ts = drm_sched_rq_get_min_vruntime(rq);
		ts = drm_sched_entity_restore_vruntime(entity, ts,
						       rq->head_prio);
	} else if (drm_sched_policy == DRM_SCHED_POLICY_RR) {
		ts = entity->rr_ts;
		/* MODIFIED: added by Zac Tawfick */
	} else if (drm_sched_policy == DRM_SCHED_POLICY_EEVDF) {
		drm_sched_entity_restore_lag(rq, entity);
		ts = drm_sched_entity_calc_vdeadline(entity);
	}

	drm_sched_rq_update_fifo_locked(entity, rq, ts);

	spin_unlock(&rq->lock);
	spin_unlock(&entity->lock);

	return sched;
}

/**
 * drm_sched_rq_remove_entity - remove an entity
 * @rq: scheduler run queue
 * @entity: scheduler entity
 *
 * Removes a scheduler entity from the run queue.
 */
void drm_sched_rq_remove_entity(struct drm_sched_rq *rq,
				struct drm_sched_entity *entity)
{
	lockdep_assert_held(&entity->lock);

	if (list_empty(&entity->list))
		return;

	spin_lock(&rq->lock);

	atomic_dec(rq->sched->score);
	list_del_init(&entity->list);

	drm_sched_rq_remove_fifo_locked(entity, rq);

	spin_unlock(&rq->lock);
}

static ktime_t drm_sched_rq_next_rr_ts(struct drm_sched_rq *rq,
				       struct drm_sched_entity *entity)
{
	ktime_t ts;

	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	ts = ktime_add_ns(rq->rr_ts, 1);
	entity->rr_ts = ts;
	rq->rr_ts = ts;

	return ts;
}

/**
 * drm_sched_rq_pop_entity - pops an entity
 * @entity: scheduler entity
 *
 * To be called every time after a job is popped from the entity.
 */
void drm_sched_rq_pop_entity(struct drm_sched_entity *entity)
{
	struct drm_sched_job *next_job;
	struct drm_sched_rq *rq;

	/*
	 * Update the entity's location in the min heap according to
	 * the timestamp of the next job, if any.
	 */
	spin_lock(&entity->lock);
	rq = entity->rq;
	spin_lock(&rq->lock);
	next_job = drm_sched_entity_queue_peek(entity);
	if (next_job) {
		ktime_t ts;

		if (drm_sched_policy == DRM_SCHED_POLICY_FAIR)
			ts = drm_sched_entity_get_job_ts(entity);
		else if (drm_sched_policy == DRM_SCHED_POLICY_FIFO)
			ts = next_job->submit_ts;
		/* MODIFIED: added by Zac Tawfick */
		else if (drm_sched_policy == DRM_SCHED_POLICY_EEVDF)
			ts = drm_sched_entity_calc_vdeadline(entity);
		else
			ts = drm_sched_rq_next_rr_ts(rq, entity);

		drm_sched_rq_update_fifo_locked(entity, rq, ts);
	} else {
		/* MODIFIED: added by Zac Tawfick */
		if (drm_sched_policy == DRM_SCHED_POLICY_EEVDF) {
			ktime_t avg_vruntime = drm_sched_rq_avg_vruntime(rq);
			drm_sched_entity_save_lag(entity, avg_vruntime);
		}

		drm_sched_rq_remove_fifo_locked(entity, rq);

		if (drm_sched_policy == DRM_SCHED_POLICY_FAIR) {
			ktime_t min_vruntime;

			min_vruntime = drm_sched_rq_get_min_vruntime(rq);
			drm_sched_entity_save_vruntime(entity, min_vruntime);
		}
	}
	spin_unlock(&rq->lock);
	spin_unlock(&entity->lock);
}

/**
 * drm_sched_rq_select_entity - Select an entity which provides a job to run
 * @sched: the gpu scheduler
 * @rq: scheduler run queue to check.
 *
 * Find oldest waiting ready entity.
 * MODIFIED: modified by Zac Tawfick
 * 
 * If policy is EEVDF, we pick the first entity with a non-positive lag value.
 * If no such entity exists, use the entity with the earliest deadline.
 * Return an entity if one is found; return an error-pointer (!NULL) if an
 * entity was ready, but the scheduler had insufficient credits to accommodate
 * its job; return NULL, if no ready entity was found.
 */
struct drm_sched_entity *
drm_sched_rq_select_entity(struct drm_gpu_scheduler *sched,
			   struct drm_sched_rq *rq)
{
	struct drm_sched_entity *best = NULL, *fallback = NULL;
	bool eevdf = drm_sched_policy == DRM_SCHED_POLICY_EEVDF;
	ktime_t avg = 0;
	struct rb_node *rb;

	spin_lock(&rq->lock);

	if (eevdf)
		avg = drm_sched_rq_avg_vruntime(rq);

	for (rb = rb_first_cached(&rq->rb_tree_root); rb; rb = rb_next(rb)) {
		struct drm_sched_entity *entity =
			rb_entry(rb, struct drm_sched_entity, rb_tree_node);

		if (!drm_sched_entity_is_ready(entity))
			continue;

		if (!eevdf) {
			best = entity;
			break;
		}
		if (!fallback)
			fallback = entity;

		/* lag >= 0  <=>  v <= V */
		if (ktime_compare(entity->vruntime, avg) <= 0) {
			best = entity;
			break;
		}
	}

	if (!best)
		best = fallback;

	if (!best) {
		spin_unlock(&rq->lock);
		return NULL;
	}

	/*
	* If we can't queue yet, preserve the current entity in terms of
	* fairness.
	*/
	if (!drm_sched_can_queue(sched, best)) {
		spin_unlock(&rq->lock);
		return ERR_PTR(-ENOSPC);
	}

	reinit_completion(&best->entity_idle);
	spin_unlock(&rq->lock);

	return best;
}
