// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/tracepoint.h>

#include "arm-smmu-v3.h"

#define NR_SLOTS 64
#define TEST_PASID 7
#define TEST_SID 1
#define Q_SHIFT 10

static bool tracefs_only;
module_param(tracefs_only, bool, 0400);

struct sample {
	u64 addr;
	u64 start;
	u64 duration;
	u32 pasid;
	u32 flags;
	u32 tag;
	u32 code;
	int cpu;
};

struct fixture {
	struct arm_smmu_device smmu;
	struct arm_smmu_master master;
	struct arm_smmu_stream stream;
	struct device *dev;
	struct dev_iommu dev_iommu;
	struct iommu_device iommu;
	struct iommu_ops ops;
	struct iommu_domain domain;
	struct iommu_attach_handle handle;
	struct iommu_fault_param param;
	struct iommu_group *group;
	struct iopf_group *pending[NR_SLOTS];
	struct sample samples[NR_SLOTS];
	spinlock_t sample_lock;
	unsigned int count;
	unsigned int callback_calls;
	u64 callback_done_ns;
	u32 evt_prod;
	bool attached;
	bool added;
	bool probe_on;
	bool reject;
	bool cleaned;
	cpumask_t saved_mask;
	bool affinity_set;
	struct tracepoint *tp;
	irqreturn_t (*evt_thread)(int, void *);
};

static void *find_function(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	void *addr;

	if (register_kprobe(&kp))
		return NULL;
	addr = kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

static void find_tracepoint(struct tracepoint *tp, void *priv)
{
	struct fixture *f = priv;

	if (!strcmp(tp->name, "iopf_latency"))
		f->tp = tp;
}

static void record_sample(void *priv, struct device *dev,
			  const struct iopf_fault *evt,
			  const struct iommu_page_response *resp,
			  u64 duration_ns)
{
	struct fixture *f = priv;
	struct sample *s;
	unsigned long flags;

	if (dev != f->dev)
		return;
	spin_lock_irqsave(&f->sample_lock, flags);
	if (f->count < NR_SLOTS) {
		s = &f->samples[f->count++];
		s->addr = evt->fault.prm.addr;
		s->start = evt->report_time_ns;
		s->duration = duration_ns;
		s->pasid = evt->fault.prm.pasid;
		s->flags = evt->fault.prm.flags;
		s->tag = resp->grpid;
		s->code = resp->code;
		s->cpu = raw_smp_processor_id();
	}
	spin_unlock_irqrestore(&f->sample_lock, flags);
}

static int set_probe(struct fixture *f, bool enabled)
{
	int ret;

	if (enabled == f->probe_on)
		return 0;
	if (enabled) {
		ret = tracepoint_probe_register(f->tp, record_sample, f);
		if (ret)
			return ret;
	} else {
		ret = tracepoint_probe_unregister(f->tp, record_sample, f);
		if (ret)
			return ret;
		tracepoint_synchronize_unregister();
	}
	f->probe_on = enabled;
	return 0;
}

static int set_pasid(struct iommu_domain *domain, struct device *dev,
		     ioasid_t pasid)
{
	return 0;
}

static void remove_pasid(struct device *dev, ioasid_t pasid,
			 struct iommu_domain *domain)
{
}

static const struct iommu_domain_ops domain_ops = {
	.set_dev_pasid = set_pasid,
};

static int save_group(struct iopf_group *group)
{
	struct fixture *f = container_of(group->attach_handle->domain,
					struct fixture, domain);
	u32 tag = group->last_fault.fault.prm.grpid;

	if (f->reject || tag >= NR_SLOTS || f->pending[tag])
		return -EIO;
	f->pending[tag] = group;
	return 0;
}

static int init_queue(struct kunit *test, struct arm_smmu_queue *q,
		      unsigned int dwords)
{
	q->llq.max_n_shift = Q_SHIFT;
	q->ent_dwords = dwords;
	q->base = kunit_kzalloc(test, (1 << Q_SHIFT) * dwords * sizeof(u64),
				GFP_KERNEL);
	q->prod_reg = (void __iomem *)kunit_kzalloc(test, sizeof(u32), GFP_KERNEL);
	q->cons_reg = (void __iomem *)kunit_kzalloc(test, sizeof(u32), GFP_KERNEL);
	return q->base && q->prod_reg && q->cons_reg ? 0 : -ENOMEM;
}

static void respond(struct fixture *f, u32 tag,
		    enum iommu_page_response_code code)
{
	struct iopf_group *group = f->pending[tag];

	if (!group)
		return;
	f->pending[tag] = NULL;
	iopf_group_response(group, code);
	iopf_free_group(group);
}

static void cleanup(void *priv)
{
	struct fixture *f = priv;
	struct iopf_fault *fault, *next;
	int i;

	if (!f || f->cleaned)
		return;
	f->cleaned = true;
	WRITE_ONCE(current->fail_nth, 0);
	for (i = 0; i < NR_SLOTS; i++)
		respond(f, i, IOMMU_PAGE_RESP_INVALID);
	list_for_each_entry_safe(fault, next, &f->param.partial, list) {
		list_del(&fault->list);
		kfree(fault);
	}
	set_probe(f, false);
	if (f->attached)
		iommu_detach_device_pasid(&f->domain, f->dev, TEST_PASID);
	if (f->added) {
		/* This fixture was not iommu_probe_device()'d. Avoid its teardown. */
		f->dev->iommu = NULL;
		iommu_group_remove_device(f->dev);
	}
	if (f->group)
		iommu_group_put(f->group);
	if (!IS_ERR_OR_NULL(f->dev)) {
		RCU_INIT_POINTER(f->dev_iommu.fault_param, NULL);
		f->dev->iommu = NULL;
		root_device_unregister(f->dev);
	}
	if (f->affinity_set)
		set_cpus_allowed_ptr(current, &f->saved_mask);
}

static void fixture_exit(struct kunit *test)
{
	/* Runs before KUnit frees the RAM queues used by pending responses. */
	cleanup(test->priv);
}

static int fixture_init(struct kunit *test)
{
	struct fixture *f;
	int ret;

	test->priv = NULL;
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	INIT_LIST_HEAD(&f->param.partial);
	spin_lock_init(&f->sample_lock);
	ret = kunit_add_action_or_reset(test, cleanup, f);
	if (ret)
		return ret;
	cpumask_copy(&f->saved_mask, current->cpus_ptr);
	ret = set_cpus_allowed_ptr(current,
				  cpumask_of(cpumask_first(cpu_online_mask)));
	if (ret)
		return ret;
	f->affinity_set = true;
	f->evt_thread = find_function("arm_smmu_evtq_thread");
	f->ops.page_response = find_function("arm_smmu_page_response");
	for_each_kernel_tracepoint(find_tracepoint, f);
	if (!f->evt_thread || !f->ops.page_response || !f->tp)
		return -ENOENT;
	f->ops.remove_dev_pasid = remove_pasid;
	f->domain.owner = &f->ops;
	f->domain.ops = &domain_ops;
	f->domain.type = IOMMU_DOMAIN_UNMANAGED;
	f->domain.iopf_handler = save_group;
	f->dev = root_device_register("smmu-iopf-latency-test");
	if (IS_ERR(f->dev))
		return PTR_ERR(f->dev);
	f->iommu.ops = &f->ops;
	f->dev_iommu.iommu_dev = &f->iommu;
	f->dev_iommu.priv = &f->master;
	f->dev_iommu.max_pasids = 256;
	mutex_init(&f->dev_iommu.lock);
	f->dev->iommu = &f->dev_iommu;
	f->param.dev = f->dev;
	mutex_init(&f->param.lock);
	refcount_set(&f->param.users, 1);
	INIT_LIST_HEAD(&f->param.partial);
	INIT_LIST_HEAD(&f->param.faults);
	rcu_assign_pointer(f->dev_iommu.fault_param, &f->param);
	f->group = iommu_group_alloc();
	if (IS_ERR(f->group)) {
		ret = PTR_ERR(f->group);
		f->group = NULL;
		return ret;
	}
	ret = iommu_group_add_device(f->group, f->dev);
	if (ret)
		return ret;
	f->added = true;
	ret = iommu_attach_device_pasid(&f->domain, f->dev, TEST_PASID,
					&f->handle);
	if (ret)
		return ret;
	f->attached = true;
	f->smmu.dev = f->dev;
	mutex_init(&f->smmu.streams_mutex);
	f->stream.id = TEST_SID;
	f->stream.master = &f->master;
	f->master.smmu = &f->smmu;
	f->master.dev = f->dev;
	f->master.stall_enabled = true;
	f->master.streams = &f->stream;
	f->master.num_streams = 1;
	f->smmu.streams = RB_ROOT;
	rb_link_node(&f->stream.node, NULL, &f->smmu.streams.rb_node);
	rb_insert_color(&f->stream.node, &f->smmu.streams);
	ret = init_queue(test, &f->smmu.evtq.q, EVTQ_ENT_DWORDS);
	if (ret)
		return ret;
	ret = init_queue(test, &f->smmu.cmdq.q, CMDQ_ENT_DWORDS);
	if (ret)
		return ret;
	f->smmu.cmdq.valid_map = kunit_kzalloc(test,
		BITS_TO_LONGS(1 << Q_SHIFT) * sizeof(atomic_long_t), GFP_KERNEL);
	if (!f->smmu.cmdq.valid_map)
		return -ENOMEM;
	atomic_set(&f->smmu.cmdq.owner_prod, 0);
	atomic_set(&f->smmu.cmdq.lock, 0);
	return 0;
}

static void send_event(struct fixture *f, u32 tag, u64 addr, bool pasid_valid)
{
	struct arm_smmu_queue *q = &f->smmu.evtq.q;
	__le64 *evt = &q->base[f->evt_prod * EVTQ_ENT_DWORDS];
	u64 word0 = FIELD_PREP(EVTQ_0_ID, EVT_ID_TRANSLATION_FAULT) |
		    FIELD_PREP(EVTQ_0_SID, TEST_SID);

	if (pasid_valid)
		word0 |= EVTQ_0_SSV | FIELD_PREP(EVTQ_0_SSID, TEST_PASID);
	evt[0] = cpu_to_le64(word0);
	evt[1] = cpu_to_le64(EVTQ_1_STALL | EVTQ_1_RnW |
			     FIELD_PREP(EVTQ_1_STAG, tag));
	evt[2] = cpu_to_le64(addr);
	evt[3] = 0;
	writel(++f->evt_prod, q->prod_reg);
	f->evt_thread(0, &f->smmu);
}

static void require_direct_mode(struct kunit *test)
{
	if (tracefs_only)
		kunit_skip(test, "direct-probe case disabled in tracefs mode");
}

static void check_clean(struct kunit *test)
{
	struct fixture *f = test->priv;

	KUNIT_EXPECT_TRUE(test, list_empty(&f->param.faults));
	KUNIT_EXPECT_EQ(test, refcount_read(&f->param.users), 1);
}

static void check_cmd(struct kunit *test, u32 tag, u32 response)
{
	struct fixture *f = test->priv;
	struct arm_smmu_queue *q = &f->smmu.cmdq.q;
	u32 index = (q->llq.prod - 1) & ((1 << Q_SHIFT) - 1);
	u64 cmd0 = le64_to_cpu(q->base[index * CMDQ_ENT_DWORDS]);
	u64 cmd1 = le64_to_cpu(q->base[index * CMDQ_ENT_DWORDS + 1]);

	KUNIT_EXPECT_EQ(test, FIELD_GET(CMDQ_0_OP, cmd0), (u64)CMDQ_OP_RESUME);
	KUNIT_EXPECT_EQ(test, FIELD_GET(CMDQ_RESUME_0_SID, cmd0), (u64)TEST_SID);
	KUNIT_EXPECT_EQ(test, FIELD_GET(CMDQ_RESUME_0_RESP, cmd0), (u64)response);
	KUNIT_EXPECT_EQ(test, FIELD_GET(CMDQ_RESUME_1_STAG, cmd1), (u64)tag);
}

static void delayed_success(struct kunit *test)
{
	struct fixture *f = test->priv;
	u64 before, reported, after;
	struct sample *s = &f->samples[0];

	require_direct_mode(test);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	before = ktime_get_ns();
	send_event(f, 1, 0x12345000, true);
	reported = ktime_get_ns();
	KUNIT_ASSERT_NOT_NULL(test, f->pending[1]);
	KUNIT_EXPECT_GE(test, f->pending[1]->last_fault.report_time_ns, before);
	KUNIT_EXPECT_LE(test, f->pending[1]->last_fault.report_time_ns, reported);
	msleep(10);
	respond(f, 1, IOMMU_PAGE_RESP_SUCCESS);
	after = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, f->count, 1U);
	KUNIT_EXPECT_EQ(test, s->addr, 0x12345000ULL);
	KUNIT_EXPECT_EQ(test, s->pasid, (u32)TEST_PASID);
	KUNIT_EXPECT_TRUE(test, s->flags & IOMMU_FAULT_PAGE_REQUEST_PASID_VALID);
	KUNIT_EXPECT_EQ(test, s->tag, 1U);
	KUNIT_EXPECT_EQ(test, s->code, (u32)IOMMU_PAGE_RESP_SUCCESS);
	KUNIT_EXPECT_GE(test, s->duration, 10000000ULL);
	KUNIT_EXPECT_LE(test, s->duration, after - before);
	check_cmd(test, 1, CMDQ_RESUME_0_RESP_RETRY);
	check_clean(test);
}

static void error_responses(struct kunit *test)
{
	struct fixture *f = test->priv;
	u64 before, after;

	require_direct_mode(test);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	before = ktime_get_ns();
	/* No no-PASID handle: core responds synchronously using the original evt. */
	send_event(f, 2, 0x2000, false);
	after = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, f->count, 1U);
	KUNIT_EXPECT_EQ(test, f->samples[0].code, (u32)IOMMU_PAGE_RESP_INVALID);
	KUNIT_EXPECT_FALSE(test, f->samples[0].flags &
				 IOMMU_FAULT_PAGE_REQUEST_PASID_VALID);
	KUNIT_EXPECT_GE(test, f->samples[0].start, before);
	KUNIT_EXPECT_LE(test, f->samples[0].duration, after - before);
	check_cmd(test, 2, CMDQ_RESUME_0_RESP_ABORT);
	send_event(f, 3, 0x3000, true);
	respond(f, 3, IOMMU_PAGE_RESP_FAILURE);
	KUNIT_ASSERT_EQ(test, f->count, 2U);
	KUNIT_EXPECT_EQ(test, f->samples[1].code, (u32)IOMMU_PAGE_RESP_FAILURE);
	check_cmd(test, 3, CMDQ_RESUME_0_RESP_ABORT);
	check_clean(test);
}

static void handler_failure(struct kunit *test)
{
	struct fixture *f = test->priv;

	require_direct_mode(test);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	f->reject = true;
	send_event(f, 4, 0x4000, true);
	KUNIT_ASSERT_EQ(test, f->count, 1U);
	KUNIT_EXPECT_EQ(test, f->samples[0].code, (u32)IOMMU_PAGE_RESP_FAILURE);
	KUNIT_EXPECT_NE(test, f->samples[0].start, 0ULL);
	check_cmd(test, 4, CMDQ_RESUME_0_RESP_ABORT);
	check_clean(test);
}

static void allocation_failure(struct kunit *test)
{
	struct fixture *f = test->priv;
	u64 before, after;

	require_direct_mode(test);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	before = ktime_get_ns();
	WRITE_ONCE(current->fail_nth, 1);
	send_event(f, 5, 0x5000, true);
	WRITE_ONCE(current->fail_nth, 0);
	after = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, f->count, 1U);
	KUNIT_EXPECT_PTR_EQ(test, f->pending[5], NULL);
	KUNIT_EXPECT_EQ(test, f->samples[0].code, (u32)IOMMU_PAGE_RESP_FAILURE);
	KUNIT_EXPECT_GE(test, f->samples[0].start, before);
	KUNIT_EXPECT_LE(test, f->samples[0].duration, after - before);
	check_cmd(test, 5, CMDQ_RESUME_0_RESP_ABORT);
	check_clean(test);
}

static void enable_transitions(struct kunit *test)
{
	struct fixture *f = test->priv;

	require_direct_mode(test);
	send_event(f, 6, 0x6000, true);
	KUNIT_ASSERT_NOT_NULL(test, f->pending[6]);
	KUNIT_EXPECT_EQ(test, f->pending[6]->last_fault.report_time_ns, 0ULL);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	respond(f, 6, IOMMU_PAGE_RESP_SUCCESS);
	KUNIT_EXPECT_EQ(test, f->count, 0U);
	send_event(f, 7, 0x7000, true);
	KUNIT_ASSERT_NOT_NULL(test, f->pending[7]);
	KUNIT_ASSERT_NE(test, f->pending[7]->last_fault.report_time_ns, 0ULL);
	KUNIT_ASSERT_EQ(test, set_probe(f, false), 0);
	respond(f, 7, IOMMU_PAGE_RESP_SUCCESS);
	check_cmd(test, 7, CMDQ_RESUME_0_RESP_RETRY);
	KUNIT_EXPECT_EQ(test, f->count, 0U);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	send_event(f, 8, 0x8000, true);
	KUNIT_ASSERT_NOT_NULL(test, f->pending[8]);
	KUNIT_ASSERT_NE(test, f->pending[8]->last_fault.report_time_ns, 0ULL);
	KUNIT_ASSERT_EQ(test, set_probe(f, false), 0);
	msleep(10);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	respond(f, 8, IOMMU_PAGE_RESP_SUCCESS);
	KUNIT_ASSERT_EQ(test, f->count, 1U);
	KUNIT_EXPECT_EQ(test, f->samples[0].tag, 8U);
	check_cmd(test, 8, CMDQ_RESUME_0_RESP_RETRY);
	KUNIT_EXPECT_GE(test, f->samples[0].duration, 10000000ULL);
	check_clean(test);
}

struct response_work {
	struct work_struct work;
	struct completion done;
	struct fixture *f;
	u32 tag;
	int cpu;
};

static void response_worker(struct work_struct *work)
{
	struct response_work *w = container_of(work, struct response_work, work);

	w->cpu = get_cpu();
	put_cpu();
	respond(w->f, w->tag, IOMMU_PAGE_RESP_SUCCESS);
	complete(&w->done);
}

static void concurrent_responses(struct kunit *test)
{
	struct fixture *f = test->priv;
	struct response_work work[8];
	u64 before[8], reported[8], starts[8], after;
	unsigned long seen = 0;
	int origin, cpu, i;

	require_direct_mode(test);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	origin = get_cpu();
	put_cpu();
	cpu = cpumask_any_but(cpu_online_mask, origin);
	KUNIT_ASSERT_LT(test, cpu, nr_cpu_ids);
	for (i = 0; i < ARRAY_SIZE(work); i++) {
		before[i] = ktime_get_ns();
		send_event(f, 10 + i, 0x10000 + i * PAGE_SIZE, true);
		reported[i] = ktime_get_ns();
		KUNIT_ASSERT_NOT_NULL(test, f->pending[10 + i]);
		starts[i] = f->pending[10 + i]->last_fault.report_time_ns;
		INIT_WORK(&work[i].work, response_worker);
		init_completion(&work[i].done);
		work[i].f = f;
		work[i].tag = 10 + i;
	}
	for (i = ARRAY_SIZE(work) - 1; i >= 0; i--)
		queue_work_on(cpu, system_wq, &work[i].work);
	for (i = 0; i < ARRAY_SIZE(work); i++) {
		wait_for_completion(&work[i].done);
		flush_work(&work[i].work);
		KUNIT_EXPECT_EQ(test, work[i].cpu, cpu);
	}
	after = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, f->count, (unsigned int)ARRAY_SIZE(work));
	for (i = 0; i < f->count; i++) {
		struct sample *s = &f->samples[i];
		u32 slot = s->tag - 10;

		KUNIT_ASSERT_LT(test, slot, (u32)ARRAY_SIZE(work));
		KUNIT_EXPECT_FALSE(test, test_and_set_bit(slot, &seen));
		KUNIT_EXPECT_EQ(test, s->addr, 0x10000ULL + slot * PAGE_SIZE);
		KUNIT_EXPECT_GE(test, s->start, before[slot]);
		KUNIT_EXPECT_LE(test, s->start, reported[slot]);
		KUNIT_EXPECT_EQ(test, s->start, starts[slot]);
		KUNIT_EXPECT_EQ(test, s->pasid, (u32)TEST_PASID);
		KUNIT_EXPECT_TRUE(test, s->flags & IOMMU_FAULT_PAGE_REQUEST_PASID_VALID);
		KUNIT_EXPECT_EQ(test, s->code, (u32)IOMMU_PAGE_RESP_SUCCESS);
		KUNIT_EXPECT_LE(test, s->duration, after - before[slot]);
		KUNIT_EXPECT_EQ(test, s->cpu, cpu);
	}
	KUNIT_EXPECT_EQ(test, seen, GENMASK(ARRAY_SIZE(work) - 1, 0));
	check_clean(test);
}

static void tag_reuse(struct kunit *test)
{
	struct fixture *f = test->priv;
	u64 before[2], reported[2], after[2];
	int i;

	require_direct_mode(test);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	before[0] = ktime_get_ns();
	send_event(f, 20, 0x20000, true);
	reported[0] = ktime_get_ns();
	respond(f, 20, IOMMU_PAGE_RESP_SUCCESS);
	after[0] = ktime_get_ns();
	before[1] = ktime_get_ns();
	send_event(f, 20, 0x21000, true);
	reported[1] = ktime_get_ns();
	respond(f, 20, IOMMU_PAGE_RESP_SUCCESS);
	after[1] = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, f->count, 2U);
	KUNIT_EXPECT_EQ(test, f->samples[0].addr, 0x20000ULL);
	KUNIT_EXPECT_EQ(test, f->samples[1].addr, 0x21000ULL);
	for (i = 0; i < 2; i++) {
		struct sample *s = &f->samples[i];

		KUNIT_EXPECT_EQ(test, s->tag, 20U);
		KUNIT_EXPECT_EQ(test, s->pasid, (u32)TEST_PASID);
		KUNIT_EXPECT_EQ(test, s->code, (u32)IOMMU_PAGE_RESP_SUCCESS);
		KUNIT_EXPECT_GE(test, s->start, before[i]);
		KUNIT_EXPECT_LE(test, s->start, reported[i]);
		KUNIT_EXPECT_LE(test, s->duration, after[i] - before[i]);
	}
	check_clean(test);
}

static void tracefs_sample(struct kunit *test)
{
	struct fixture *f = test->priv;

	if (!tracefs_only)
		kunit_skip(test, "run separately with tracefs_only=1");
	send_event(f, 30, 0xabcde000, true);
	KUNIT_ASSERT_NOT_NULL(test, f->pending[30]);
	msleep(10);
	respond(f, 30, IOMMU_PAGE_RESP_SUCCESS);
	check_clean(test);
}

static void generic_page_response(struct device *dev, struct iopf_fault *evt,
				  struct iommu_page_response *resp)
{
	struct fixture *f = container_of(dev_iommu_priv_get(dev),
					struct fixture, master);

	f->callback_calls++;
	msleep(10);
	f->callback_done_ns = ktime_get_ns();
}

static void generic_response(struct kunit *test)
{
	struct fixture *f = test->priv;
	struct iopf_fault evt = {
		.fault.type = IOMMU_FAULT_PAGE_REQ,
		.fault.prm = {
			.flags = IOMMU_FAULT_PAGE_REQUEST_PASID_VALID |
				 IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE,
			.pasid = TEST_PASID,
			.grpid = 40,
			.addr = 0x40000,
		},
		.report_time_ns = U64_MAX,
	};
	u64 before, reported, after;
	struct sample *s = &f->samples[0];

	require_direct_mode(test);
	f->ops.page_response = generic_page_response;
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	before = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, iommu_report_device_fault(f->dev, &evt), 0);
	reported = ktime_get_ns();
	KUNIT_ASSERT_NOT_NULL(test, f->pending[40]);
	KUNIT_EXPECT_GE(test, f->pending[40]->last_fault.report_time_ns, before);
	KUNIT_EXPECT_LE(test, f->pending[40]->last_fault.report_time_ns, reported);
	respond(f, 40, IOMMU_PAGE_RESP_SUCCESS);
	after = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, f->count, 1U);
	KUNIT_EXPECT_EQ(test, f->callback_calls, 1U);
	KUNIT_EXPECT_EQ(test, s->tag, 40U);
	KUNIT_EXPECT_EQ(test, s->addr, 0x40000ULL);
	KUNIT_EXPECT_EQ(test, s->pasid, (u32)TEST_PASID);
	KUNIT_EXPECT_TRUE(test, s->flags & IOMMU_FAULT_PAGE_REQUEST_PASID_VALID);
	KUNIT_EXPECT_EQ(test, s->code, (u32)IOMMU_PAGE_RESP_SUCCESS);
	KUNIT_EXPECT_GE(test, s->duration, 10000000ULL);
	KUNIT_EXPECT_LE(test, s->duration, after - before);
	KUNIT_EXPECT_GE(test, s->start + s->duration, f->callback_done_ns);
	/* No SMMUv3 response callback or command queue was used. */
	KUNIT_EXPECT_EQ(test, f->smmu.cmdq.q.llq.prod, 0U);
	check_clean(test);
}

static void partial_group(struct kunit *test)
{
	struct fixture *f = test->priv;
	struct iopf_fault evt = {
		.fault.type = IOMMU_FAULT_PAGE_REQ,
		.fault.prm = {
			.flags = IOMMU_FAULT_PAGE_REQUEST_PASID_VALID,
			.pasid = TEST_PASID,
			.grpid = 41,
			.addr = 0x41000,
		},
	};
	u64 first, before, reported, after;

	require_direct_mode(test);
	KUNIT_ASSERT_EQ(test, set_probe(f, true), 0);
	KUNIT_ASSERT_EQ(test, iommu_report_device_fault(f->dev, &evt), 0);
	first = evt.report_time_ns;
	KUNIT_ASSERT_NE(test, first, 0ULL);
	KUNIT_EXPECT_PTR_EQ(test, f->pending[41], NULL);
	KUNIT_EXPECT_FALSE(test, list_empty(&f->param.partial));
	KUNIT_EXPECT_EQ(test, f->count, 0U);
	msleep(10);
	evt.fault.prm.flags |= IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE;
	evt.fault.prm.addr = 0x42000;
	before = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, iommu_report_device_fault(f->dev, &evt), 0);
	reported = ktime_get_ns();
	KUNIT_ASSERT_NOT_NULL(test, f->pending[41]);
	KUNIT_EXPECT_EQ(test, f->pending[41]->fault_count, (size_t)2);
	KUNIT_EXPECT_TRUE(test, list_empty(&f->param.partial));
	KUNIT_EXPECT_GE(test, f->pending[41]->last_fault.report_time_ns, before);
	KUNIT_EXPECT_LE(test, f->pending[41]->last_fault.report_time_ns, reported);
	respond(f, 41, IOMMU_PAGE_RESP_SUCCESS);
	after = ktime_get_ns();
	KUNIT_ASSERT_EQ(test, f->count, 1U);
	KUNIT_EXPECT_EQ(test, f->samples[0].addr, 0x42000ULL);
	KUNIT_EXPECT_EQ(test, f->samples[0].tag, 41U);
	KUNIT_EXPECT_EQ(test, f->samples[0].start, evt.report_time_ns);
	KUNIT_EXPECT_GT(test, f->samples[0].start, first);
	KUNIT_EXPECT_LE(test, f->samples[0].duration, after - before);
	check_cmd(test, 41, CMDQ_RESUME_0_RESP_RETRY);
	check_clean(test);
}

static void kabi_layout(struct kunit *test)
{
	struct legacy_iopf_fault {
		struct iommu_fault fault;
		struct list_head list;
		KABI_RESERVE(1)
		KABI_RESERVE(2)
		KABI_RESERVE(3)
		KABI_RESERVE(4)
	};

	require_direct_mode(test);
	KUNIT_EXPECT_EQ(test, sizeof(struct iopf_fault),
			sizeof(struct legacy_iopf_fault));
	KUNIT_EXPECT_EQ(test, offsetof(struct iopf_fault, report_time_ns),
			offsetof(struct legacy_iopf_fault, kabi_reserved1));
}

static struct kunit_case cases[] = {
	KUNIT_CASE(kabi_layout),
	KUNIT_CASE(delayed_success),
	KUNIT_CASE(error_responses),
	KUNIT_CASE(handler_failure),
	KUNIT_CASE(allocation_failure),
	KUNIT_CASE(enable_transitions),
	KUNIT_CASE(concurrent_responses),
	KUNIT_CASE(tag_reuse),
	KUNIT_CASE(generic_response),
	KUNIT_CASE(partial_group),
	KUNIT_CASE(tracefs_sample),
	{}
};

static struct kunit_suite suite = {
	.name = "smmu-iopf-latency",
	.init = fixture_init,
	.exit = fixture_exit,
	.test_cases = cases,
};

kunit_test_suite(suite);
MODULE_LICENSE("GPL");
