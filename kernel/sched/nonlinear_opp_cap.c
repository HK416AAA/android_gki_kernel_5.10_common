// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Oplus. All rights reserved.
 */


#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <../kernel/sched/sched.h>
#include <trace/hooks/topology.h>
#include "uag_trace.h"
#include "cpufreq_schedutil.h"

/************************************
 * Structure && Global data
 ************************************/
#define MAX_PD_COUNT (3)
#define CAPACITY_ENTRY_SIZE (0x2)

struct pd_capacity_info {
	int nr_caps;
	unsigned int *util_opp;
	unsigned int *util_freq;
	unsigned long *caps;
	struct cpumask cpus;
};

static struct pd_capacity_info *pd_capacity_tbl;
static int pd_count;
static int entry_count;

unsigned int util_scale = 1280;
unsigned int sysctl_sched_capacity_margin_dvfs = 20;

/*
 * set sched capacity margin for DVFS, Default = 20
 */
int set_sched_capacity_margin_dvfs(unsigned int capacity_margin)
{
	if (capacity_margin < 0 || capacity_margin > 95)
		return -1;

	sysctl_sched_capacity_margin_dvfs = capacity_margin;
	util_scale = (SCHED_CAPACITY_SCALE * 100 / (100 - sysctl_sched_capacity_margin_dvfs));

	return 0;
}

static inline bool __nonlinear_used(void)
{
	return true;
}

bool nonlinear_used(void)
{
	return __nonlinear_used();
}

unsigned long pd_get_opp_capacity(int cpu, int opp)
{
	int i;
	struct pd_capacity_info *pd_info;

	for (i = 0; i < pd_count; i++) {
		pd_info = &pd_capacity_tbl[i];

		if (!cpumask_test_cpu(cpu, &pd_info->cpus))
			continue;

		/* Return max capacity if opp is not valid */
		if (opp < 0 || opp >= pd_info->nr_caps)
			return pd_info->caps[0];

		return pd_info->caps[opp];
	}

	/* Should NOT reach here */
	return 0;
}

static void nlopp_arch_set_freq_scale(void *data, const struct cpumask *cpus,
		unsigned long freq, unsigned long max, unsigned long *scale)
{
	int cpu = cpumask_first(cpus);
	int opp;
	unsigned long cap, max_cap;
	struct cpufreq_policy *policy;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return;

	opp = cpufreq_frequency_table_get_index(policy, freq);
	cpufreq_cpu_put(policy);
	if (opp < 0)
		return;

	cap = pd_get_opp_capacity(cpu, opp);
	max_cap = pd_get_opp_capacity(cpu, 0);

	*scale = SCHED_CAPACITY_SCALE * cap / max_cap;
}

unsigned int pd_get_nr_caps(int cluster_id)
{
	return pd_capacity_tbl[cluster_id].nr_caps;
}

unsigned int pd_get_cpu_freq(int cpu, int idx)
{
	struct em_perf_domain *pd;

	if (!__nonlinear_used())
		return 0;

	pd = em_cpu_get(cpu);
	if (!pd) {
		pr_err("pd is null\n");
		return 0;
	}

	return (unsigned int)pd->table[idx].frequency;
}

#ifdef CONFIG_OPLUS_UAG_USE_TL
static bool capacity_margin_dvfs_changed;

void set_capacity_margin_dvfs_changed_uag(bool changed)
{
	capacity_margin_dvfs_changed = changed;
}
EXPORT_SYMBOL(set_capacity_margin_dvfs_changed_uag);

#ifdef CONFIG_OPLUS_MULTI_LV_TL
static void uag_choose_multi_util(struct sugov_policy *sg_policy, unsigned long *target_util)
{
	int i;

	*target_util = 0;
	for (i = 0; i < UA_UTIL_SIZE; i++) {
		if (!sg_policy->multi_util[i])
			continue;

		sg_policy->multi_util_type = i;
		sg_policy->multi_util[i] = choose_util(sg_policy, sg_policy->multi_util[i]);
		if (*target_util < sg_policy->multi_util[i])
			*target_util = sg_policy->multi_util[i];
	}
	trace_uag_choose_multi_util(sg_policy, *target_util);

	if (sg_policy->multi_util[UA_UTIL_FBG] > sg_policy->multi_util[UA_UTIL_SYS])
		sg_policy->fbg_gt_sys_cnt++;
	sg_policy->total_cnt++;

	/* reset multi_util */
	for (i = 0; i < UA_UTIL_SIZE; i++)
		sg_policy->multi_util[i] = 0;
}
#endif

void update_util_tl(struct sugov_policy *sg_policy, unsigned long *util)
{
	unsigned long target_util;

	if (sg_policy && !capacity_margin_dvfs_changed) {
#ifndef CONFIG_OPLUS_MULTI_LV_TL
		target_util = choose_util(sg_policy, *util);
#else
		if (!sg_policy->tunables->multi_tl_enable)
			target_util = choose_util(sg_policy, *util);
		else
			uag_choose_multi_util(sg_policy, &target_util);
#endif /* CONFIG_OPLUS_MULTI_LV_TL */
		if (target_util >= 0)
			*util = target_util;
		trace_uag_next_util_tl(sg_policy->policy->cpu, *util, arch_scale_cpu_capacity(sg_policy->policy->cpu), target_util);
	} else {
		*util = (*util * util_scale) >> SCHED_CAPACITY_SHIFT;
	}
}

void uag_update_util(void *data, unsigned long util, unsigned long freq,
		unsigned long cap, unsigned long *max_util, struct cpufreq_policy *policy,
		bool *need_freq_update)
{
	unsigned int first_cpu;
	int cluster_id;
	struct sugov_policy *sg_policy;

	first_cpu = cpumask_first(policy->related_cpus);
	cluster_id = topology_physical_package_id(first_cpu);

	if (cluster_id >= MAX_CLUSTERS)
		return;

	if (init_flag[cluster_id] == 0)
		return;

	sg_policy = policy->governor_data;

	if (sg_policy) {
		update_util_tl(sg_policy, max_util);
	}
}
#endif /* CONFIG_OPLUS_UAG_USE_TL */

void nlopp_map_util_freq(void *data, unsigned long util, unsigned long freq,
				struct cpumask *cpumask, unsigned long *next_freq)
{
	int i, j, cap;
	struct pd_capacity_info *info;
	unsigned long temp_util;
#ifdef CONFIG_OPLUS_UAG_USE_TL
	struct sugov_policy *sg_policy = (struct sugov_policy *)data;
	unsigned long raw_util = util;
	unsigned long raw_freq;
#endif /* CONFIG_OPLUS_UAG_USE_TL */

	temp_util = util;

#ifdef CONFIG_OPLUS_UAG_USE_TL
	raw_util = (raw_util * util_scale) >> SCHED_CAPACITY_SHIFT;

	update_util_tl(sg_policy, &util);
#else
	util = (util * util_scale) >> SCHED_CAPACITY_SHIFT;
#endif /* CONFIG_OPLUS_UAG_USE_TL */

	for (i = 0; i < pd_count; i++) {
		info = &pd_capacity_tbl[i];
		if (!cpumask_equal(cpumask, &info->cpus))
			continue;
		cap = info->caps[info->nr_caps - 1];
		util = min(util, info->caps[info->nr_caps - 1]);
		j = info->util_opp[util];
		*next_freq = info->util_freq[util];
		trace_uag_next_freq_info(i, util, j, info->util_freq[util]);
#ifdef CONFIG_OPLUS_UAG_USE_TL
		raw_util = min(raw_util, info->caps[info->nr_caps - 1]);
		raw_freq = info->util_freq[raw_util];
		if (sg_policy)
			trace_uag_next_freq_tl(sg_policy->policy->cpu, raw_util, raw_freq, util, *next_freq);
#endif /* CONFIG_OPLUS_UAG_USE_TL */
		break;
	}

	if (data != NULL) {
		struct sugov_policy *sg_policy = (struct sugov_policy *)data;
		struct cpufreq_policy *policy = sg_policy->policy;

		if (sg_policy->need_freq_update) {
			unsigned int idx_min, idx_max;
			unsigned int min_freq, max_freq;

			idx_min = cpufreq_frequency_table_target(policy, policy->min,
						CPUFREQ_RELATION_L);
			idx_max = cpufreq_frequency_table_target(policy, policy->max,
						CPUFREQ_RELATION_H);
			min_freq = policy->freq_table[idx_min].frequency;
			max_freq = policy->freq_table[idx_max].frequency;
			*next_freq = clamp_val(*next_freq, min_freq, max_freq);
			j = clamp_val(j, idx_min, idx_max);
		}
		policy->cached_target_freq = *next_freq;
		policy->cached_resolved_idx = j;
		sg_policy->cached_raw_freq = map_util_freq(temp_util, freq, cap);
	}
}

/************************************
 * Initialization function
 ************************************/
static void free_capacity_table(void)
{
	int i;

	if (!pd_capacity_tbl)
		return;

	for (i = 0; i < pd_count; i++) {
		kfree(pd_capacity_tbl[i].caps);
		kfree(pd_capacity_tbl[i].util_opp);
		kfree(pd_capacity_tbl[i].util_freq);
	}
	kfree(pd_capacity_tbl);
	pd_capacity_tbl = NULL;
}

unsigned long cluster2_cap[21] = {1024, 962, 937, 906, 869, 832, 795, 758, 721, 684,
				  641, 604, 567, 530, 493, 450, 413, 376, 333, 296,
				  252};

unsigned long cluster1_cap[20] = {832, 802, 779, 744,
				  709, 674, 640, 605, 570, 535, 500, 471, 436, 401,
				  366, 337, 302, 267, 232, 192};

unsigned long cluster0_cap[16] = {277, 263, 247, 229, 213, 197, 182, 168, 155,
				  145, 129, 110, 94, 76, 60, 41};
				  
static int init_capacity_table(void)
{
	int i, j, cpu;
	int count = 0;
	unsigned long cap;
	unsigned long end_cap;
	long next_cap, k;
	struct pd_capacity_info *pd_info;
	struct em_perf_domain *pd;
	unsigned long *c_cap;

	for (i = 0; i < pd_count; i++) {
		pd_info = &pd_capacity_tbl[i];
		cpu = cpumask_first(&pd_info->cpus);
		pd = em_cpu_get(cpu);

		if (!pd)
			goto err;

		if (i == 0) {
			c_cap = cluster0_cap;
		} else if (i == 1) {
			c_cap = cluster1_cap;
		} else if (i == 2) {
			c_cap = cluster2_cap;
		} else {
			pr_info("capacity table init failed with i = %d, pd_count = %d\n", i, pd_count);
			goto err;
		}

		for (j = 0; j < pd_info->nr_caps; j++) {
			cap = c_cap[j];
			if (j == pd_info->nr_caps - 1) {
				next_cap = -1;
			} else {
				next_cap = c_cap[j + 1];
			}

			if (cap == 0 || next_cap == 0)
				goto err;

			pd_info->caps[j] = cap;

			if (!pd_info->util_opp) {
				pd_info->util_opp = kcalloc(cap + 1, sizeof(unsigned int),
										GFP_KERNEL);
				if (!pd_info->util_opp)
					goto nomem;
			}

			if (!pd_info->util_freq) {
				pd_info->util_freq = kcalloc(cap + 1, sizeof(unsigned int),
										GFP_KERNEL);
				if (!pd_info->util_freq)
					goto nomem;
			}


			for (k = cap; k > next_cap; k--) {
				pd_info->util_opp[k] = j;
				pd_info->util_freq[k] =
					pd->table[pd->nr_perf_states - j - 1].frequency;
			}

			count += 1;
		}
		end_cap = c_cap[j-1];
		if (end_cap != cap)
			goto err;

		for_each_cpu(j, &pd_info->cpus) {
			if (per_cpu(cpu_scale, j) != pd_info->caps[0]) {
				pr_info("per_cpu(cpu_scale, %d)=%d, pd_info->caps[0]=%d\n",
					j, per_cpu(cpu_scale, j), pd_info->caps[0]);
				per_cpu(cpu_scale, j) = pd_info->caps[0];
			}
		}
	}

	if (entry_count != count)
		goto err;

	return 0;

nomem:
	pr_info("allocate util mapping table failed\n");
err:
	pr_info("count %d does not match entry_count %d\n", count, entry_count);

	free_capacity_table();
	return -ENOENT;
}

static int alloc_capacity_table(void)
{
	int i;
	int ret = 0;
	int cur_tbl = 0;

	pd_capacity_tbl = kcalloc(MAX_PD_COUNT, sizeof(struct pd_capacity_info),
			GFP_KERNEL);
	if (!pd_capacity_tbl)
		return -ENOMEM;

	for (i = 0; i < nr_cpu_ids; i++) {
		int nr_caps;
		struct em_perf_domain *pd;

		pd = em_cpu_get(i);
		if (!pd) {
			pr_info("em_cpu_get return NULL for cpu#%d", i);
			continue;
		}
		if (i != cpumask_first(to_cpumask(pd->cpus)))
			continue;

		WARN_ON(cur_tbl >= MAX_PD_COUNT);

		nr_caps = pd->nr_perf_states;
		pd_capacity_tbl[cur_tbl].nr_caps = nr_caps;
		cpumask_copy(&pd_capacity_tbl[cur_tbl].cpus, to_cpumask(pd->cpus));
		pd_capacity_tbl[cur_tbl].caps = kcalloc(nr_caps, sizeof(unsigned long),
							GFP_KERNEL);
		if (!pd_capacity_tbl[cur_tbl].caps)
			goto nomem;

		pd_capacity_tbl[cur_tbl].util_opp = NULL;
		pd_capacity_tbl[cur_tbl].util_freq = NULL;

		entry_count += nr_caps;

		cur_tbl++;
	}

	pd_count = cur_tbl;

	return 0;

nomem:
	ret = -ENOMEM;
	free_capacity_table();

	return ret;
}

static int pd_capacity_tbl_show(struct seq_file *m, void *v)
{
	int i, j;
	struct pd_capacity_info *pd_info;

	for (i = 0; i < MAX_PD_COUNT; i++) {
		pd_info = &pd_capacity_tbl[i];

		if (!pd_info->nr_caps)
			break;

		seq_printf(m, "Pd table: %d\n", i);
		seq_printf(m, "nr_caps: %d\n", pd_info->nr_caps);
		seq_printf(m, "cpus: %*pbl\n", cpumask_pr_args(&pd_info->cpus));
		for (j = 0; j < pd_info->nr_caps; j++)
			seq_printf(m, "%d: %lu\n", j, pd_info->caps[j]);
	}

	return 0;
}

static int pd_capacity_tbl_open(struct inode *in, struct file *file)
{
	return single_open(file, pd_capacity_tbl_show, NULL);
}

static const struct proc_ops pd_capacity_tbl_ops = {
	.proc_open = pd_capacity_tbl_open,
	.proc_read = seq_read,
	.proc_lseek = default_llseek,
};

int init_opp_cap_info(struct proc_dir_entry *dir)
{
	int ret = 0;
	struct proc_dir_entry *entry;

	if (pd_capacity_tbl)
		return ret;

	ret = alloc_capacity_table();
	if (ret)
		return ret;

	ret = init_capacity_table();
	if (ret)
		return ret;

	if (dir == NULL)
		dir = proc_mkdir("schedutil", NULL);

	if (dir == NULL)
		pr_err("mkdir proc/uag failed!\n");

	entry = proc_create("pd_capacity_tbl", 0644, dir, &pd_capacity_tbl_ops);
	if (!entry)
		pr_err("proc/uag/pd_capacity_tbl entry create failed\n");

	ret = register_trace_android_vh_arch_set_freq_scale(nlopp_arch_set_freq_scale, NULL);
	if (ret)
		return ret;

	return ret;
}

void clear_opp_cap_info(void)
{
	free_capacity_table();
}
