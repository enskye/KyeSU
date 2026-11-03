// getpriority/setpriority with PRIO_USER and ioprio_get/ioprio_set with
// IOPRIO_WHO_USER fail with ESRCH only when no task runs as the given uid, so
// any app can probe whether another app (e.g. a root manager) is running.
// When enabled, such a lookup from a seccomp-confined app about another app's
// uid finds no user, and the syscall fails with ESRCH.
//
// All four syscalls resolve a foreign uid through find_user(), and only on
// that path, so the "bl find_user" in each of them is redirected to our
// wrapper. Every other call, and the compat ABI sharing these bodies, runs
// untouched. Logic ported from aerath-stuff/fgetpriority (lkm-raw-branch).

#define GP_HIDE_APP_START	10000
#define GP_HIDE_ISOLATED_START	90000

// (uid_t)-1 is unmappable and owns no tasks, so find_user() returns NULL for it
#define GP_HIDE_INVALID_UID	((uid_t)-1)

static const char *const gp_hide_callsites[] = {
	"__arm64_sys_getpriority",
	"__arm64_sys_setpriority",
	"__arm64_sys_ioprio_get",
	"__arm64_sys_ioprio_set",
};

static uintptr_t gp_hide_callsite_addr[ARRAY_SIZE(gp_hide_callsites)] __read_mostly;
static uintptr_t gp_hide_find_user_addr __read_mostly;

// How the hidden path is reached. Chosen once at init: raw "bl find_user"
// patching when every callsite resolved, else a kprobe on find_user, else
// unsupported (feature left unregistered).
enum gp_hide_mode {
	GP_HIDE_MODE_NONE = 0,
	GP_HIDE_MODE_RAW,
	GP_HIDE_MODE_KPROBE,
};

static enum gp_hide_mode gp_hide_mode __read_mostly = GP_HIDE_MODE_NONE;

static const char *gp_hide_mode_name(enum gp_hide_mode mode)
{
	switch (mode) {
	case GP_HIDE_MODE_RAW:
		return "raw branch patch";
	case GP_HIDE_MODE_KPROBE:
		return "kprobe";
	default:
		return "none";
	}
}

static DEFINE_MUTEX(getpriority_hide_mutex);
static bool getpriority_hide_enabled = false;

static __always_inline uid_t gp_hide_appid(uid_t uid)
{
	uid %= PER_USER_RANGE;

	// an isolated process in a user namespace acts for the namespace owner
	if (unlikely(uid >= GP_HIDE_ISOLATED_START)) {
		struct user_namespace *ns = current_user_ns();
		if (ns != &init_user_ns) {
			kuid_t owner = make_kuid(ns, 0);
			if (uid_valid(owner))
				uid = from_kuid(&init_user_ns, owner);
		}
	}

	return uid;
}

static __always_inline bool gp_hide_should_hide(uid_t who)
{
	if (who < GP_HIDE_APP_START)
		return false;

	if (!ksu_is_seccomp_enabled())
		return false;

	uid_t cuid = current_uid().val;
	if (cuid < GP_HIDE_APP_START)
		return false;

	return gp_hide_appid(cuid) != gp_hide_appid(who);
}

// Called instead of find_user() from the patched syscalls. The lookup is done
// either way so a hidden uid takes as long as a visible one; the reference is
// dropped here, as the callers skip free_uid() on NULL.
static noinline struct user_struct *ksu_gp_hide_find_user(kuid_t uid)
{
	struct user_struct *user = find_user(uid);

	if (unlikely(user && gp_hide_should_hide(__kuid_val(uid)))) {
		free_uid(user);
		return nullptr;
	}

	return user;
}

static int gp_hide_patch(size_t i, uintptr_t from, uintptr_t to)
{
	uintptr_t site = gp_hide_callsite_addr[i];

	int ret = arm64_bl_patch(site, ksu_get_ksym_size(site, 256 * sizeof(uint32_t)), from, to);
	pr_info("getpriority_hide: %s: ret: %d\n", gp_hide_callsites[i], ret);
	return ret;
}

// All or nothing: a failed site rolls back the ones already patched
static int getpriority_hide_hook(void)
{
	uintptr_t hook = (uintptr_t)&ksu_gp_hide_find_user;
	size_t i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(gp_hide_callsites); i++) {
		ret = gp_hide_patch(i, gp_hide_find_user_addr, hook);
		if (ret)
			break;
	}

	if (!ret)
		return 0;

	while (i--)
		(void)gp_hide_patch(i, hook, gp_hide_find_user_addr);

	return ret;
}

static void getpriority_hide_unhook(void)
{
	uintptr_t hook = (uintptr_t)&ksu_gp_hide_find_user;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(gp_hide_callsites); i++)
		(void)gp_hide_patch(i, hook, gp_hide_find_user_addr);
}

#ifdef CONFIG_KPROBES
// Fallback when the callsites could not be resolved for raw patching. The probe
// sits on find_user() itself and, for a call that should be hidden, rewrites the
// incoming uid to an unmappable one so the real find_user() returns NULL.
static struct kprobe *gp_hide_kp;

static int gp_hide_find_user_pre(struct kprobe *p, struct pt_regs *regs)
{
	uid_t *uid = (uid_t *)&PT_REGS_PARM1(regs);

	if (gp_hide_should_hide(*uid))
		*uid = GP_HIDE_INVALID_UID;

	return 0;
}

static int getpriority_hide_kp_hook(void)
{
	gp_hide_kp = init_kprobe("find_user", gp_hide_find_user_pre);
	return gp_hide_kp ? 0 : -EAGAIN;
}

static void getpriority_hide_kp_unhook(void)
{
	destroy_kprobe(&gp_hide_kp);
}
#else
static int getpriority_hide_kp_hook(void) { return -ENOSYS; }
static void getpriority_hide_kp_unhook(void) {}
#endif

static int getpriority_hide_enable(void)
{
	if (gp_hide_mode == GP_HIDE_MODE_KPROBE)
		return getpriority_hide_kp_hook();

	return getpriority_hide_hook();
}

static void getpriority_hide_disable(void)
{
	if (gp_hide_mode == GP_HIDE_MODE_KPROBE)
		getpriority_hide_kp_unhook();
	else
		getpriority_hide_unhook();
}

static int getpriority_hide_feature_get(u64 *value)
{
	*value = READ_ONCE(getpriority_hide_enabled) ? 1 : 0;
	return 0;
}

static int getpriority_hide_feature_set(u64 value)
{
	bool enable = value != 0;

	guarded_mutex_lock(&getpriority_hide_mutex);

	if (enable == getpriority_hide_enabled)
		return 0;

	if (enable) {
		int ret = getpriority_hide_enable();
		if (ret)
			return ret;
	} else {
		getpriority_hide_disable();
	}

	WRITE_ONCE(getpriority_hide_enabled, enable);
	pr_info("getpriority_hide: set to %d\n", enable);
	return 0;
}

static const struct ksu_feature_handler getpriority_hide_handler = {
	.feature_id = KSU_FEATURE_GETPRIORITY_HIDE,
	.name = "getpriority_hide",
	.get_handler = getpriority_hide_feature_get,
	.set_handler = getpriority_hide_feature_set,
};

// Prefer raw branch patching; it needs find_user and all four callsites.
static bool __init gp_hide_raw_available(void)
{
	size_t i;

	gp_hide_find_user_addr = kallsyms_lookup_retry("find_user");
	if (!gp_hide_find_user_addr)
		return false;

	for (i = 0; i < ARRAY_SIZE(gp_hide_callsites); i++) {
		gp_hide_callsite_addr[i] = kallsyms_lookup_retry(gp_hide_callsites[i]);
		if (!gp_hide_callsite_addr[i])
			return false;
	}

	return true;
}

// Left unregistered, so reported as unsupported, when no mechanism is available
static void __init ksu_getpriority_hide_init(void)
{
	if (gp_hide_raw_available())
		gp_hide_mode = GP_HIDE_MODE_RAW;
	else if (IS_ENABLED(CONFIG_KPROBES))
		gp_hide_mode = GP_HIDE_MODE_KPROBE;
	else
		gp_hide_mode = GP_HIDE_MODE_NONE;

	pr_info("getpriority_hide: mechanism: %s\n", gp_hide_mode_name(gp_hide_mode));

	if (gp_hide_mode == GP_HIDE_MODE_NONE)
		return;

	if (ksu_register_feature_handler(&getpriority_hide_handler))
		pr_err("Failed to register getpriority_hide feature handler\n");
}
