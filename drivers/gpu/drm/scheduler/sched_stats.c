#include <drm/gpu_scheduler.h>
#include <linux/ktime.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#include "sched_internal.h"

struct drm_sched_entity_stats *alloc_entity_stats(void)
{
	struct drm_sched_entity_stats *stats = kzalloc_obj(*stats);
	if (!stats)
		return NULL;

	kref_init(&stats->kref);
	spin_lock_init(&stats->lock);

	return stats;
}

inline void drm_sched_stats_get(struct drm_sched_entity_stats *stats)
{
	kref_get(&stats->kref);
}

static void drm_sched_stats_release(struct kref *kref)
{
	struct drm_sched_entity_stats *stats =
		container_of(kref, struct drm_sched_entity_stats, kref);

	kfree(stats);
}

inline void drm_sched_stats_put(struct drm_sched_entity_stats *stats)
{
	kref_put(&stats->kref, drm_sched_stats_release);
}

void drm_sched_stats_update_runtime(struct drm_sched_entity_stats *stats,
				    ktime_t delta)
{
	spin_lock(&stats->lock);

	stats->runtime += delta;
	stats->v_runtime += ktime_to_ns(delta);

	spin_unlock(&stats->lock);
}

void drm_sched_stats_update_deadline(struct drm_sched_entity_stats *stats)
{
	spin_lock(&stats->lock);

	stats->deadline = stats->v_runtime + stats->slice;

	spin_unlock(&stats->lock);
}
