// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 \xx
 *
 * This file is a downstream extension and NOT affiliated, endorsed by,
 * or maintained by the official KernelSU developers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

// selinux hide, arm64 only, mostly handlers. thas has random callbacks around

#include <linux/jhash.h>

// callbacks
static void ksu_init_hook_transaction_ops_write(void);
static void ksu_init_hook_sel_handle_status_ops_open(void);

static void ksu_decide_inode_setxattr(bool sh_hooked);
static void ksu_decide_setprocattr(bool sh_hooked);
static bool ksu_selinux_hide_enabled __read_mostly;
static struct page *ksu_fake_status_page __read_mostly;

#if defined(CONFIG_ARM64) && defined(CONFIG_KALLSYMS)

// flag to check
#define KSU_HAS_ARM64_SELINUX_HIDE

#define __ksym_size(callsite)	ksu_get_ksym_size(callsite, (256 * sizeof(uint32_t)))
#define patch_arm64_branches_trampolined(callsite, symbol_addr, hook)	\
	arm64_branch_patch_trampolined(callsite, __ksym_size(callsite), symbol_addr, (uintptr_t)hook, true)

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0) && defined(KSU_COMPAT_HAS_SELINUX_STATE)
__attribute__((used)) struct page *sh_selinux_kernel_status_page(struct selinux_state *state)
#else
__attribute__((used)) struct page *sh_selinux_kernel_status_page(void)
#endif
{
	if (unlikely(!ksu_selinux_hide_enabled))
		goto orig_page;

	if (!ksu_is_seccomp_enabled())
		goto orig_page;

	if (current_uid().val < 10000)
		goto orig_page;

	assume(!!ksu_fake_status_page);
	if (unlikely(!ksu_fake_status_page))
		goto orig_page;

	pr_info("selinux_hide: selinux_kernel_status_page: served fake_page\n");
	return ksu_fake_status_page;

orig_page:
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0) && defined(KSU_COMPAT_HAS_SELINUX_STATE)
	return selinux_kernel_status_page(&selinux_state);
#else
	return selinux_kernel_status_page();
#endif
}

static void patch_branch_sel_open_handle_status(void)
{
	uintptr_t target_callsite = kallsyms_lookup_retry("sel_open_handle_status");
	uintptr_t symbol_addr = kallsyms_lookup_retry("selinux_kernel_status_page");
	if (!symbol_addr || !target_callsite)
		goto hook_classic;

	int ret = patch_arm64_branches_trampolined(target_callsite, symbol_addr, (uintptr_t)&sh_selinux_kernel_status_page);
	pr_info("selinux_hide: hook_site: sel_open_handle_status->selinux_kernel_status_page ret: %d \n", ret);
	if (!ret)
		return;
hook_classic:
	pr_info("selinux_hide: fallback to function pointer hook!\n");
	ksu_init_hook_sel_handle_status_ops_open();
}

// Use the live policy snapshot so unknown application inputs share the same
// invalid-context path as types hidden from that application.
static bool sh_unknown_context_type(const char *context, size_t len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	if (!context || !len)
		return false;
	const char *end = context + len;
	const char *role = memchr(context, ':', len);
	const char *type = role ? memchr(role + 1, ':', end - role - 1) : NULL;
	if (!type)
		return false;
	type++;
	const char *type_end = memchr(type, ':', end - type);
	if (!type_end || type_end == type || type_end - type >= 128)
		return false;

	char name[128];
	memcpy(name, type, type_end - type);
	name[type_end - type] = '\0';
	rcu_read_lock();
	struct selinux_policy *policy = rcu_dereference(selinux_state.policy);
	bool unknown = policy && !ksu_exists(&policy->policydb, name);
	rcu_read_unlock();
	return unknown;
#else
	return false;
#endif
}

#define SH_INVALID_CONTEXT "u:r:?:s0"

static bool sh_reject_context_uncached(const char *context, size_t len, bool normalize_unknown)
{
	bool unknown = normalize_unknown && sh_unknown_context_type(context, len);
	const char *visible = unknown ? SH_INVALID_CONTEXT : context;
	size_t visible_len = unknown ? sizeof(SH_INVALID_CONTEXT) - 1 : len;
	bool hidden = ksu_should_destroy_context((char *)visible, visible_len);

	return unknown | hidden;
}

// A bounded per-CPU cache avoids repeating policy and hide-list searches for
// common context queries. Policy replacement and list publication invalidate it.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#define SH_CONTEXT_CACHE_BUCKETS 16

struct sh_context_cache_key {
	u32 len;
	char context[124];
};

struct sh_context_cache_entry {
	struct sh_context_cache_key key;
	const struct selinux_policy *policy;
	u64 generation;
	u32 policy_seqno;
	bool valid;
	bool reject;
};

struct sh_context_cache_bucket {
	struct sh_context_cache_entry entries[2];
	unsigned int next;
};

struct sh_context_cache {
	struct sh_context_cache_bucket buckets[SH_CONTEXT_CACHE_BUCKETS];
};

static DEFINE_PER_CPU(struct sh_context_cache, sh_context_cache);

static bool sh_context_cache_lookup(const struct sh_context_cache_key *key,
				    unsigned int bucket_index,
				    const struct selinux_policy *policy,
				    u32 seqno, u64 generation, bool *reject)
{
	unsigned long flags;
	struct sh_context_cache *cache = get_cpu_ptr(&sh_context_cache);
	local_irq_save(flags);
	struct sh_context_cache_bucket *bucket = &cache->buckets[bucket_index];
	bool found = false;

	for (size_t i = 0; i < ARRAY_SIZE(bucket->entries); i++) {
		const struct sh_context_cache_entry *entry = &bucket->entries[i];
		bool same_key = !memcmp(&entry->key, key, sizeof(*key));
		bool match = same_key & entry->valid & (entry->policy == policy) &
			(entry->policy_seqno == seqno) & (entry->generation == generation);
		if (match)
			*reject = entry->reject;
		found |= match;
	}
	local_irq_restore(flags);
	put_cpu_ptr(&sh_context_cache);
	return found;
}

static void sh_context_cache_store(const struct sh_context_cache_key *key,
				   unsigned int bucket_index,
				   const struct selinux_policy *policy,
				   u32 seqno, u64 generation, bool reject)
{
	unsigned long flags;
	struct sh_context_cache *cache = get_cpu_ptr(&sh_context_cache);
	local_irq_save(flags);
	struct sh_context_cache_bucket *bucket = &cache->buckets[bucket_index];
	struct sh_context_cache_entry *entry = &bucket->entries[bucket->next];

	entry->key = *key;
	entry->policy = policy;
	entry->policy_seqno = seqno;
	entry->generation = generation;
	entry->reject = reject;
	entry->valid = true;
	bucket->next ^= 1;
	local_irq_restore(flags);
	put_cpu_ptr(&sh_context_cache);
}
#endif

static bool sh_reject_context(const char *context, size_t len, bool normalize_unknown)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	struct sh_context_cache_key key = { 0 };
	if (!context || !normalize_unknown || len > sizeof(key.context))
		return sh_reject_context_uncached(context, len, normalize_unknown);

	u64 generation = atomic64_read_acquire(&ksu_hide_list_generation);
	if (generation & 1)
		return sh_reject_context_uncached(context, len, normalize_unknown);

	key.len = len;
	memcpy(key.context, context, len);
	unsigned int bucket_index = jhash(&key, sizeof(key), 0) &
		(SH_CONTEXT_CACHE_BUCKETS - 1);
	bool reject = false;

	rcu_read_lock();
	struct selinux_policy *policy = rcu_dereference(selinux_state.policy);
	if (!policy) {
		rcu_read_unlock();
		return sh_reject_context_uncached(context, len, normalize_unknown);
	}
	u32 seqno = READ_ONCE(policy->latest_granting);
	bool found = sh_context_cache_lookup(&key, bucket_index, policy,
					    seqno, generation, &reject);
	rcu_read_unlock();
	if (found && atomic64_read_acquire(&ksu_hide_list_generation) == generation)
		return reject;

	reject = sh_reject_context_uncached(context, len, normalize_unknown);
	// A concurrent policy/list update must not publish a stale cached answer.
	rcu_read_lock();
	struct selinux_policy *current_policy = rcu_dereference(selinux_state.policy);
	if (current_policy == policy && current_policy &&
	    READ_ONCE(current_policy->latest_granting) == seqno &&
	    atomic64_read_acquire(&ksu_hide_list_generation) == generation)
		sh_context_cache_store(&key, bucket_index, policy, seqno, generation, reject);
	rcu_read_unlock();
	return reject;
#else
	return sh_reject_context_uncached(context, len, normalize_unknown);
#endif
}

// Preserve the context layout while giving the parser an unknown type.
static void sh_mask_context_type(char *context, size_t len)
{
	char *end = context + len;
	char *role = memchr(context, ':', len);
	char *type = role ? memchr(role + 1, ':', end - role - 1) : NULL;
	if (!type)
		return;
	type++;
	char *type_end = memchr(type, ':', end - type);
	if (type_end)
		memset(type, '?', type_end - type);
}

// A hidden type or a hidden source/target pair invalidates the whole query.
// Check the context fields before altering the first context in place.
static void sh_mask_context_types(char *buf, size_t len)
{
	if (!ksu_should_destroy_context(buf, len))
		return;

	char *end = buf + len;
	char *context = buf;
	while (context < end && *context == ' ')
		context++;
	char *context_end = memchr(context, ' ', end - context);
	if (!context_end)
		context_end = end;
	sh_mask_context_type(context, context_end - context);
}

static char *sh_simple_transaction_get(struct file *file, const char __user *buf, size_t size)
{
	char *data = simple_transaction_get(file, buf, size);

	if (IS_ERR(data) || !ksu_selinux_hide_enabled ||
	    !ksu_is_seccomp_enabled() || current_uid().val < 10000)
		return data;

#define SEL_CONTEXT 5
#define SEL_ACCESS 6
	ino_t ino = file_inode(file)->i_ino;
	size_t context_len = ino == SEL_CONTEXT ? strnlen(data, size) : size;
	if (ino == SEL_CONTEXT && context_len >= sizeof(SH_INVALID_CONTEXT) &&
	    sh_reject_context(data, context_len, true)) {
		// simple_transaction_get owns a zero-terminated kernel buffer.
		memset(data, 0, size);
		memcpy(data, SH_INVALID_CONTEXT, sizeof(SH_INVALID_CONTEXT));
	} else if (ino == SEL_CONTEXT) {
		sh_mask_context_types(data, context_len);
	} else if (ino == SEL_ACCESS) {
		size_t len = size;
		char *contexts = ksu_selinux_access_contexts(data, &len);
		if (contexts)
			sh_mask_context_types(contexts, len);
	}
	return data;
}

#if 0
static void sh_simple_transaction_set(struct file *file, size_t n)
{
	simple_transaction_set(file, n);

	if (unlikely(!ksu_selinux_hide_enabled))
		return;

	if (!ksu_is_seccomp_enabled())
		return;

	if (current_uid().val < 10000)
		return;

	ino_t ino = file_inode(file)->i_ino;
	if (ino != SEL_ACCESS)
		return;

	// simple_transaction_get()
	struct simple_transaction_argresp *ar = file->private_data;
	if (!ar)
		return;

	uint32_t avd_allowed, ff, avd_auditallow, avd_auditdeny, avd_seqno, avd_flags;
	if (sscanf(ar->data, "%x %x %x %x %u %x", &avd_allowed, &ff, &avd_auditallow, &avd_auditdeny, &avd_seqno, &avd_flags) != 6)
		return;

	if (IS_ENABLED(CONFIG_KSU_DEBUG))
		pr_info("ar->data: before: %s \n", ar->data);

	avd_seqno = 1; // simple_transaction_set()
	ar->size = scnprintf(ar->data, SIMPLE_TRANSACTION_LIMIT, "%x %x %x %x %u %x", avd_allowed, ff, avd_auditallow, avd_auditdeny, avd_seqno, avd_flags);

	if (IS_ENABLED(CONFIG_KSU_DEBUG))
		pr_info("ar->data: after: %s \n", ar->data);
}
#endif // 0

static inline void sh_swap_seqno(struct av_decision *avd)
{
	if (unlikely(!ksu_selinux_hide_enabled))
		return;
	if (!ksu_is_seccomp_enabled())
		return;
	if (current_uid().val < 10000)
		return;
	if (!avd)
		return;
	avd->seqno = 1;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0) && defined(KSU_COMPAT_HAS_SELINUX_STATE)
static void sh_security_compute_av_user(struct selinux_state *state, u32 ssid, u32 tsid, u16 tclass, struct av_decision *avd)
{
	security_compute_av_user(state, ssid, tsid, tclass, avd);
	sh_swap_seqno(avd);
}
#else
static void sh_security_compute_av_user(u32 ssid, u32 tsid, u16 tclass, struct av_decision *avd)
{
	security_compute_av_user(ssid, tsid, tclass, avd);
	sh_swap_seqno(avd);
}
#endif

static void patch_branches_selinux_transaction_write(void)
{
	uintptr_t target_callsite = kallsyms_lookup_retry("selinux_transaction_write");
	uintptr_t symbol_addr = kallsyms_lookup_retry("simple_transaction_get");
	if (!symbol_addr || !target_callsite)
		goto hook_classic;

	uintptr_t trampoline = 0x0;
	int ret = arm64_branch_patch_trampolined_reusable(&trampoline, target_callsite, symbol_addr, (uintptr_t)&sh_simple_transaction_get);
	pr_info("selinux_hide: hook_site: selinux_transaction_write->simple_transaction_get ret: %d \n", ret);
	if (ret)
		goto hook_classic;

#if 0
	symbol_addr = kallsyms_lookup_retry("simple_transaction_set");
	ret = patch_arm64_branches_trampolined(target_callsite, symbol_addr, (uintptr_t)&sh_simple_transaction_set);
	pr_info("selinux_hide: hook_site: selinux_transaction_write->simple_transaction_set ret: %d \n", ret);
	if (!ret)
		return;

	/**
	 * on LTO + CONFIG_MODULES=n this can fail. (EXPORT_SYMBOL becomes useless, compiler inlines simple_transaction_set)
	 * 
	 * inline, melted.
	 * ffffff80104ea128: f100041f     	cmp	x0, #0x1
	 * ffffff80104ea12c: 54fffdab     	b.lt	0xffffff80104ea0e0 <selinux_transaction_write+0x30>
	 * ffffff80104ea130: f13fe41f     	cmp	x0, #0xff9
	 * ffffff80104ea134: 540000c2     	b.hs	0xffffff80104ea14c <selinux_transaction_write+0x9c>
	 * ffffff80104ea138: f9406688     	ldr	x8, [x20, #0xc8]	// ar = file->private_data; 
	 * ffffff80104ea13c: d5033bbf     	dmb	ish			// smp_mb()
	 * ffffff80104ea140: f9000100     	str	x0, [x8]		// ar->size = n;
	 * ffffff80104ea144: aa1303e0     	mov	x0, x19
	 * ffffff80104ea148: 17ffffe6     	b	0xffffff80104ea0e0 <selinux_transaction_write+0x30>
	 * ffffff80104ea14c: d4210000     	brk	#0x800 			// BUG_ON()
	 * ffffff80104ea150: 14000000     	b	0xffffff80104ea150 <selinux_transaction_write+0xa0>
	 *
	 * non-inlined here, as normal.
	 * ffffff8010470194: f100041f     	cmp	x0, #0x1
	 * ffffff8010470198: 540000ab     	b.lt	0xffffff80104701ac <selinux_transaction_write+0x8c>
	 * ffffff801047019c: aa1403e0     	mov	x0, x20
	 * ffffff80104701a0: 97f8d990     	bl	0xffffff80102a67e0 <simple_transaction_set>
	 *
	 * however lets be opportunistic here, we keep the code, maybe we can get lucky
	 * so if second hook fails, we just gc the first one and fallback to fnptr
	 */
#else
	target_callsite = kallsyms_lookup_retry("sel_write_access");
	symbol_addr = kallsyms_lookup_retry("security_compute_av_user");
	if (!symbol_addr || !target_callsite)
		goto hook_classic;

	ret = patch_arm64_branches_trampolined(target_callsite, symbol_addr, (uintptr_t)&sh_security_compute_av_user);
	pr_info("selinux_hide: hook_site: sel_write_access->security_compute_av_user ret: %d \n", ret);
	if (!ret)
		return;
#endif
	symbol_addr = kallsyms_lookup_retry("simple_transaction_get");
	assume(!!symbol_addr);
	if (!!trampoline)
		ret = arm64_b_or_bl_patch(target_callsite, ksu_get_ksym_size(target_callsite, 32 * 4), trampoline, symbol_addr);
	else
		ret = arm64_b_or_bl_patch(target_callsite, ksu_get_ksym_size(target_callsite, 32 * 4), (uintptr_t)&sh_simple_transaction_get, symbol_addr);
	pr_info("selinux_hide: unhook_site: selinux_transaction_write->transaction_get ret: %d \n", ret);
	if (ret) // uh oh, indeterminate state, just warn and limp on
		pr_info("selinux_hide: unhook_site: selinux_transaction_write->transaction_get unhook failed! indeterminate state!\n");

hook_classic:
	pr_info("selinux_hide: fallback to function pointer hook!\n");
	ksu_init_hook_transaction_ops_write();
}

static void *security_context_to_sid_core_fn __read_mostly = nullptr;
static __always_inline bool should_ret_einval(const char *scontext, u32 scontext_len, bool normalize_unknown)
{
	if (unlikely(!ksu_selinux_hide_enabled))
		return false;

	if (!ksu_is_seccomp_enabled())
		return false;

	if (current_uid().val < 10000)
		return false;

	if (!sh_reject_context(scontext, scontext_len, normalize_unknown))
		return false;

	if (IS_ENABLED(CONFIG_KSU_DEBUG))
		pr_info("selinux_hide: security_context_to_sid: destroy: %.*s\n", scontext_len, scontext);
	return true;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0) && defined(KSU_COMPAT_HAS_SELINUX_STATE)
static __nocfi int sh_security_context_to_sid_core(struct selinux_state *state, const char *scontext, u32 scontext_len, u32 *sid, u32 def_sid, gfp_t gfp_flags, int force)
{
	bool masked = should_ret_einval(scontext, scontext_len, !force);
	if (masked) {
		scontext = SH_INVALID_CONTEXT;
		scontext_len = sizeof(SH_INVALID_CONTEXT) - 1;
		force = 0;
	}
	int ret = ((typeof (sh_security_context_to_sid_core) *)security_context_to_sid_core_fn)(state, scontext, scontext_len, sid, def_sid, gfp_flags, force);
	return ret;
}
#else
static __nocfi int sh_security_context_to_sid_core(const char *scontext, u32 scontext_len, u32 *sid, u32 def_sid, gfp_t gfp_flags, int force)
{
	bool masked = should_ret_einval(scontext, scontext_len, !force);
	if (masked) {
		scontext = SH_INVALID_CONTEXT;
		scontext_len = sizeof(SH_INVALID_CONTEXT) - 1;
		force = 0;
	}
	int ret = ((typeof (sh_security_context_to_sid_core) *)security_context_to_sid_core_fn)(scontext, scontext_len, sid, def_sid, gfp_flags, force);
	return ret;
}
#endif

#define SECURITY_CONTEXT_TO_SID_sstate	struct selinux_state *state, const char *scontext, u32 scontext_len, u32 *sid, gfp_t gfp
__overloaded static int sh_security_context_to_sstate(SECURITY_CONTEXT_TO_SID_sstate)
{
	assume(!!&security_context_to_sid);
	bool masked = should_ret_einval(scontext, scontext_len, true);
	if (masked) {
		scontext = SH_INVALID_CONTEXT;
		scontext_len = sizeof(SH_INVALID_CONTEXT) - 1;
	}
	int ret = ((typeof(sh_security_context_to_sstate) *)(void *)security_context_to_sid)(state, scontext, scontext_len, sid, gfp);
	return ret;
}

#define SECURITY_CONTEXT_TO_SID_gfp	const char *scontext, u32 scontext_len, u32 *sid, gfp_t gfp
__overloaded static int sh_security_context_to_sid_gfp(SECURITY_CONTEXT_TO_SID_gfp)
{
	assume(!!&security_context_to_sid);
	bool masked = should_ret_einval(scontext, scontext_len, true);
	if (masked) {
		scontext = SH_INVALID_CONTEXT;
		scontext_len = sizeof(SH_INVALID_CONTEXT) - 1;
	}
	int ret = ((typeof(sh_security_context_to_sid_gfp) *)(void *)security_context_to_sid)(scontext, scontext_len, sid, gfp);
	return ret;
}

#define SECURITY_CONTEXT_TO_SID_legacy	const char *scontext, u32 scontext_len, u32 *sid
__overloaded static int sh_security_context_to_sid_legacy(SECURITY_CONTEXT_TO_SID_legacy)
{
	assume(!!&security_context_to_sid);
	bool masked = should_ret_einval(scontext, scontext_len, true);
	if (masked) {
		scontext = SH_INVALID_CONTEXT;
		scontext_len = sizeof(SH_INVALID_CONTEXT) - 1;
	}
	int ret = ((typeof(sh_security_context_to_sid_legacy) *)(void *)security_context_to_sid)(scontext, scontext_len, sid);
	return ret;
}

#define sh_security_context_to_sid_overloaded _Generic(					\
(security_context_to_sid),								\
	int(*)(SECURITY_CONTEXT_TO_SID_sstate): sh_security_context_to_sstate,		\
	int(*)(SECURITY_CONTEXT_TO_SID_gfp): 	sh_security_context_to_sid_gfp,		\
	int(*)(SECURITY_CONTEXT_TO_SID_legacy): sh_security_context_to_sid_legacy	\
)

static inline int patch_branch_context_to_sid_inode_setxattr(void)
{
	uintptr_t target_callsite = kallsyms_lookup_retry("selinux_inode_setxattr");
	uintptr_t symbol_addr = kallsyms_lookup_retry("security_context_to_sid_core");
	if (!symbol_addr || !target_callsite)
		goto noncore_xattr;

	if (!security_context_to_sid_core_fn)
		security_context_to_sid_core_fn = (void *)symbol_addr;

	int ret = patch_arm64_branches_trampolined(target_callsite, symbol_addr, (uintptr_t)&sh_security_context_to_sid_core);
	pr_info("selinux_hide: hook_site: selinux_inode_setxattr->security_context_to_sid_core ret: %d \n", ret);
	if (!ret)
		goto success_xattr;
noncore_xattr:;
	target_callsite = kallsyms_lookup_retry("selinux_inode_setxattr");
	symbol_addr = kallsyms_lookup_retry("security_context_to_sid");
	if (!symbol_addr || !target_callsite)
		goto failure_xattr;

	ret = patch_arm64_branches_trampolined(target_callsite, symbol_addr, (uintptr_t)&sh_security_context_to_sid_overloaded);
	pr_info("selinux_hide: hook_site: selinux_inode_setxattr->security_context_to_sid ret: %d \n", ret);
	if (ret)
		goto failure_xattr;
success_xattr:
	ksu_decide_inode_setxattr(true);
	return ret;

failure_xattr:
	ksu_decide_inode_setxattr(false);
	return ret;
}

static inline int patch_branch_context_to_sid_procattr(void)
{
	uintptr_t target_callsite = 0x0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	if (!target_callsite) 
		target_callsite = kallsyms_lookup_retry("selinux_lsm_setattr", lookup_noscan);
#endif
	if (!target_callsite)
		target_callsite = kallsyms_lookup_retry("selinux_setprocattr");
	uintptr_t symbol_addr = kallsyms_lookup_retry("security_context_to_sid_core");
	if (!symbol_addr || !target_callsite)
		goto noncore_procattr;

	if (!security_context_to_sid_core_fn)
		security_context_to_sid_core_fn = (void *)symbol_addr;

	int ret = patch_arm64_branches_trampolined(target_callsite, symbol_addr, (uintptr_t)&sh_security_context_to_sid_core);
	pr_info("selinux_hide: hook_site: selinux_setprocattr->security_context_to_sid_core ret: %d \n", ret);
	if (!ret)
		goto success_procattr;
noncore_procattr:;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	if (!target_callsite)
		target_callsite = kallsyms_lookup_retry("selinux_lsm_setattr", lookup_noscan);
#endif
	if (!target_callsite)
		target_callsite = kallsyms_lookup_retry("selinux_setprocattr");
	symbol_addr = kallsyms_lookup_retry("security_context_to_sid");
	if (!symbol_addr || !target_callsite)
		goto failure_procattr;

	ret = patch_arm64_branches_trampolined(target_callsite, symbol_addr, (uintptr_t)&sh_security_context_to_sid_overloaded);
	pr_info("selinux_hide: hook_site: selinux_setprocattr->security_context_to_sid ret: %d \n", ret);
	if (ret)
		goto failure_procattr;
success_procattr:
	// Retain the outer current-attribute normalizer as well as the parser hook.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
	ksu_decide_setprocattr(false);
#else
	ksu_decide_setprocattr(true);
#endif
	return ret;
failure_procattr:
	ksu_decide_setprocattr(false);
	return ret;
}

static inline int patch_branch_context_to_sid(void)
{
	patch_branch_context_to_sid_inode_setxattr();
	patch_branch_context_to_sid_procattr();
	return 0;
}

static inline void init_arm64_selinux_hide(void)
{
	pr_info("selinux_hide: init aarch64 dedicated implementation! \n");
	patch_branch_sel_open_handle_status();
	patch_branches_selinux_transaction_write();
	patch_branch_context_to_sid();
}

#undef patch_arm64_branches_trampolined
#undef __ksym_size

#endif // ARM64 && KALLSYMS
