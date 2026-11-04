package me.weishu.kernelsu.ui.util

import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import me.weishu.kernelsu.Natives
import java.io.FileDescriptor
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

/**
 * Package change sequence from the kernel. It moves after every packages.list rewrite,
 * so it covers installs and removals from any source. Stays at 0 on kernels without it.
 */
object PackageEvents {
    private val _sequence = MutableStateFlow(0)
    val sequence: StateFlow<Int> = _sequence.asStateFlow()

    private var started = false
    private var eventFd: ParcelFileDescriptor? = null

    @Synchronized
    fun start() {
        if (started) return
        val fd = Natives.getPackageEventFd()
        if (fd < 0) {
            // Kernels without the ioctl answer ENOTTY; don't ask them again.
            started = fd == -OsConstants.ENOTTY
            return
        }
        started = true
        val pfd = ParcelFileDescriptor.adoptFd(fd).also { eventFd = it }
        val buf = ByteArray(Int.SIZE_BYTES)
        try {
            readSequence(pfd.fileDescriptor, buf)
        } catch (_: ErrnoException) {
            return
        }
        thread(name = "ksu-pkg-events", isDaemon = true) { watch(pfd.fileDescriptor, buf) }
    }

    private fun readSequence(fd: FileDescriptor, buf: ByteArray) {
        if (Os.read(fd, buf, 0, buf.size) == buf.size) {
            _sequence.value = ByteBuffer.wrap(buf).order(ByteOrder.nativeOrder()).int
        }
    }

    private fun watch(fd: FileDescriptor, buf: ByteArray) {
        val pollFds = arrayOf(StructPollfd().apply {
            this.fd = fd
            events = OsConstants.POLLIN.toShort()
        })
        while (true) {
            try {
                Os.poll(pollFds, -1)
                readSequence(fd, buf)
            } catch (e: ErrnoException) {
                if (e.errno != OsConstants.EINTR) return
            }
        }
    }
}
