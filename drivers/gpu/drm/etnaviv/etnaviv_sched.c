/*
 * Copyright (C) 2017 Etnaviv Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kthread.h>

#include "etnaviv_drv.h"
#include "etnaviv_dump.h"
#include "etnaviv_gem.h"
#include "etnaviv_gpu.h"
#include "etnaviv_sched.h"

static int etnaviv_job_hang_limit = 0;
module_param_named(job_hang_limit, etnaviv_job_hang_limit, int , 0444);
static int etnaviv_hw_jobs_limit = 2;
module_param_named(hw_job_limit, etnaviv_hw_jobs_limit, int , 0444);

static struct dma_fence *
etnaviv_sched_dependency(struct drm_sched_job *sched_job,
			 struct drm_sched_entity *entity)
{
	struct etnaviv_gem_submit *submit = to_etnaviv_submit(sched_job);
	struct dma_fence *fence;
	int i;

	if (unlikely(submit->in_fence)) {
		fence = submit->in_fence;
		submit->in_fence = NULL;

		if (!dma_fence_is_signaled(fence))
			return fence;

		dma_fence_put(fence);
	}

	for (i = 0; i < submit->nr_bos; i++) {
		struct etnaviv_gem_submit_bo *bo = &submit->bos[i];
		int j;

		if (bo->excl) {
			fence = bo->excl;
			bo->excl = NULL;

			if (!dma_fence_is_signaled(fence))
				return fence;

			dma_fence_put(fence);
		}

		for (j = 0; j < bo->nr_shared; j++) {
			if (!bo->shared[j])
				continue;

			fence = bo->shared[j];
			bo->shared[j] = NULL;

			if (!dma_fence_is_signaled(fence))
				return fence;

			dma_fence_put(fence);
		}
		kfree(bo->shared);
		bo->nr_shared = 0;
		bo->shared = NULL;
	}

	return NULL;
}

static struct dma_fence *etnaviv_sched_run_job(struct drm_sched_job *sched_job)
{
	struct etnaviv_gem_submit *submit = to_etnaviv_submit(sched_job);
	struct dma_fence *fence = NULL;

	if (likely(!sched_job->s_fence->finished.error))
		fence = etnaviv_gpu_submit(submit);
	else
		dev_dbg(submit->gpu->dev, "skipping bad job\n");

	return fence;
}

static void etnaviv_sched_timedout_job(struct drm_sched_job *sched_job)
{
	struct etnaviv_gem_submit *submit = to_etnaviv_submit(sched_job);
	struct etnaviv_gpu *gpu = submit->gpu;

	/* block scheduler */
	kthread_park(gpu->sched.thread);
	drm_sched_hw_job_reset(&gpu->sched, sched_job);

	/* get the GPU back into the init state */
	etnaviv_core_dump(gpu);
	etnaviv_gpu_recover_hang(gpu);

	/* restart scheduler after GPU is usable again */
	drm_sched_job_recovery(&gpu->sched);
	kthread_unpark(gpu->sched.thread);
}

static void etnaviv_sched_free_job(struct drm_sched_job *sched_job)
{
	struct etnaviv_gem_submit *submit = to_etnaviv_submit(sched_job);

	etnaviv_submit_put(submit);
}

static const struct drm_sched_backend_ops etnaviv_sched_ops = {
	.dependency = etnaviv_sched_dependency,
	.run_job = etnaviv_sched_run_job,
	.timedout_job = etnaviv_sched_timedout_job,
	.free_job = etnaviv_sched_free_job,
};

int etnaviv_sched_push_job(struct drm_sched_entity *sched_entity,
			   struct etnaviv_gem_submit *submit)
{
	int ret;

	ret = drm_sched_job_init(&submit->sched_job, &submit->gpu->sched,
				 sched_entity, submit->cmdbuf.ctx);
	if (ret)
		return ret;

	submit->out_fence = dma_fence_get(&submit->sched_job.s_fence->finished);
	mutex_lock(&submit->gpu->fence_idr_lock);
	submit->out_fence_id = idr_alloc_cyclic(&submit->gpu->fence_idr,
						submit->out_fence, 0,
						INT_MAX, GFP_KERNEL);
	mutex_unlock(&submit->gpu->fence_idr_lock);
	if (submit->out_fence_id < 0)
		return -ENOMEM;

	/* the scheduler holds on to the job now */
	kref_get(&submit->refcount);

	drm_sched_entity_push_job(&submit->sched_job, sched_entity);

	return 0;
}

int etnaviv_sched_init(struct etnaviv_gpu *gpu)
{
	int ret;

	ret = drm_sched_init(&gpu->sched, &etnaviv_sched_ops,
			     etnaviv_hw_jobs_limit, etnaviv_job_hang_limit,
			     msecs_to_jiffies(500), dev_name(gpu->dev));
	if (ret)
		return ret;

	return 0;
}

void etnaviv_sched_fini(struct etnaviv_gpu *gpu)
{
	drm_sched_fini(&gpu->sched);
}
