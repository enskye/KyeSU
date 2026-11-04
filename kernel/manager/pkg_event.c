// SPDX-License-Identifier: GPL-2.0-only

/*
 * Package change events for the manager.
 *
 * The rename observer calls this on every packages.list rewrite, so it marks
 * one package change. The fd turns readable when the global sequence moved past
 * the one this fd last read; read() returns the current sequence as a u32.
 */

static atomic_t ksu_pkg_event_seq = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(ksu_pkg_event_wq);

static void ksu_pkg_event_notify(void)
{
	atomic_inc(&ksu_pkg_event_seq);
	wake_up_interruptible_poll(&ksu_pkg_event_wq, EPOLLIN | EPOLLRDNORM);
}

static ssize_t ksu_pkg_event_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	u32 seq = (u32)atomic_read(&ksu_pkg_event_seq);

	if (count < sizeof(seq))
		return -EINVAL;

	if (copy_to_user(buf, &seq, sizeof(seq)))
		return -EFAULT;

	file->private_data = (void *)(uintptr_t)seq;
	return sizeof(seq);
}

static unsigned __bitwise ksu_pkg_event_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &ksu_pkg_event_wq, wait);

	if ((u32)(uintptr_t)file->private_data != (u32)atomic_read(&ksu_pkg_event_seq))
		return POLLIN | POLLRDNORM;

	return 0;
}

static const struct file_operations ksu_pkg_event_fops = {
	.owner = THIS_MODULE,
	.read = ksu_pkg_event_read,
	.poll = ksu_pkg_event_poll,
	.llseek = noop_llseek,
};

static int ksu_install_pkg_event_fd(void)
{
	void *seq = (void *)(uintptr_t)(u32)atomic_read(&ksu_pkg_event_seq);

	return anon_inode_getfd("[ksu_pkg_event]", &ksu_pkg_event_fops, seq, O_RDONLY | O_CLOEXEC);
}
