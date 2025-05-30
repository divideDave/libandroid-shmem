//#include <android/log.h>
#define _POSIX_C_SOURCE 200809L // DJM added this to resolve implisit decl compiler errors. Work around feature macros in system headers. 
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <paths.h>
#include <fcntl.h>      // DJM open(), O_RDWR
#include <sys/ioctl.h>  // DJM ioctl()

#define __u32 uint32_t
#include "ashmem.h"
#include "shm.h"

#define DBG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define ASHV_KEY_SYMLINK_PATH _PATH_TMP "ashv_key_%d"
#define ANDROID_SHMEM_SOCKNAME "/dev/shm/%08x"
#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp) ({         \
    __typeof__ (exp) _rc;                      \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })
#endif

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	// The shmid (shared memory id) contains the socket address (16 bits)
	// and a local id (15 bits).
	int id;
	void *addr;
	int descriptor;
	size_t size;
	bool markedForDeletion;
	key_t key;
} shmem_t;

static shmem_t* shmem = NULL;
static size_t shmem_amount = 0;

// The lower 16 bits of (getpid() + i), where i is a sequence number.
// It is unique among processes as it's only set when bound.
static int ashv_local_socket_id = 0;
// To handle forks we store which pid the ashv_local_socket_id was
// created for.
static int ashv_pid_setup = 0;
static pthread_t ashv_listening_thread_id = 0;

static int ancil_send_fd(int sock, int fd)
{
	DBG("%s" , __PRETTY_FUNCTION__);
	char nothing = '!';
	struct iovec nothing_ptr = { .iov_base = &nothing, .iov_len = 1 };

	struct {
		struct cmsghdr align;
		int fd[1];
	} ancillary_data_buffer;

	struct msghdr message_header = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &nothing_ptr,
		.msg_iovlen = 1,
		.msg_flags = 0,
		.msg_control = &ancillary_data_buffer,
		.msg_controllen = sizeof(struct cmsghdr) + sizeof(int)
	};

	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message_header);
	cmsg->cmsg_len = message_header.msg_controllen; // sizeof(int);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	((int*) CMSG_DATA(cmsg))[0] = fd;

	return sendmsg(sock, &message_header, 0) >= 0 ? 0 : -1;
}

static int ancil_recv_fd(int sock)
{
	DBG("%s" , __PRETTY_FUNCTION__);
	char nothing = '!';
	struct iovec nothing_ptr = { .iov_base = &nothing, .iov_len = 1 };

	struct {
		struct cmsghdr align;
		int fd[1];
	} ancillary_data_buffer;

	struct msghdr message_header = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &nothing_ptr,
		.msg_iovlen = 1,
		.msg_flags = 0,
		.msg_control = &ancillary_data_buffer,
		.msg_controllen = sizeof(struct cmsghdr) + sizeof(int)
	};

	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message_header);
	cmsg->cmsg_len = message_header.msg_controllen;
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	((int*) CMSG_DATA(cmsg))[0] = -1;

	if (recvmsg(sock, &message_header, 0) < 0) return -1;

	return ((int*) CMSG_DATA(cmsg))[0];
}

static int ashmem_get_size_region(int fd)
{
	DBG("%s" , __PRETTY_FUNCTION__);
	//int ret = __ashmem_is_ashmem(fd, 1);
	//if (ret < 0) return ret;
	return TEMP_FAILURE_RETRY(ioctl(fd, ASHMEM_GET_SIZE, NULL));
}

/*
 * From https://android.googlesource.com/platform/system/core/+/master/libcutils/ashmem-dev.c
 *
 * ashmem_create_region - creates a new named ashmem region and returns the file
 * descriptor, or <0 on error.
 *
 * `name' is the label to give the region (visible in /proc/pid/maps)
 * `size' is the size of the region, in page-aligned bytes
 */
static int ashmem_create_region(char const* name, size_t size)
{
	DBG("%s" , __PRETTY_FUNCTION__);
	int fd = open("/dev/ashmem", O_RDWR);
	if (fd < 0) return fd;

	char name_buffer[ASHMEM_NAME_LEN] = {0};
	strncpy(name_buffer, name, sizeof(name_buffer));
	name_buffer[sizeof(name_buffer)-1] = 0;

	int ret = ioctl(fd, ASHMEM_SET_NAME, name_buffer);
	if (ret < 0) {
        DBG("ioctl ASHMEM_SET_NAME fails in %s\n",__FUNCTION__);
		perror("ioctl ASHMEM_SET_NAME");
        goto error;
    }
    

	ret = ioctl(fd, ASHMEM_SET_SIZE, size);
	if (ret < 0) goto error;

	return fd;
error:
	close(fd);
	return ret;
}

static void ashv_check_pid()
{
	DBG("%s" , __PRETTY_FUNCTION__);
	pid_t mypid = getpid();
	if (ashv_pid_setup == 0) {
		ashv_pid_setup = mypid;
	} else if (ashv_pid_setup != mypid) {
		DBG("%s: Cleaning to new pid=%d from oldpid=%d", __PRETTY_FUNCTION__, mypid, ashv_pid_setup);
		// We inherited old state across a fork.
		ashv_pid_setup = mypid;
		ashv_local_socket_id = 0;
		ashv_listening_thread_id = 0;
		shmem_amount = 0;
		// Unlock if fork left us with held lock from parent thread.
		pthread_mutex_unlock(&mutex);
		if (shmem != NULL) free(shmem);
		shmem = NULL;
	}
}


// Store index in the lower 15 bits and the socket id in the
// higher 16 bits.
static int ashv_shmid_from_counter(unsigned int counter)
{
	DBG("%s" , __PRETTY_FUNCTION__);
	return ashv_local_socket_id * 0x10000 + counter;
}

static int ashv_socket_id_from_shmid(int shmid)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	return shmid / 0x10000;
}

static int ashv_find_local_index(int shmid)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	for (size_t i = 0; i < shmem_amount; i++)
		if (shmem[i].id == shmid)
			return i;
	return -1;
}

static void* ashv_thread_function(void* arg)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	int sock = *(int*)arg;
	free(arg);
	struct sockaddr_un addr;
	socklen_t len = sizeof(addr);
	int sendsock;
	DBG("%s: thread started", __PRETTY_FUNCTION__);
	while ((sendsock = accept(sock, (struct sockaddr *)&addr, &len)) != -1) {
		int shmid;
		if (recv(sendsock, &shmid, sizeof(shmid), 0) != sizeof(shmid)) {
			DBG("%s: ERROR: recv() returned not %zu bytes", __PRETTY_FUNCTION__, sizeof(shmid));
			close(sendsock);
			continue;
		}
		pthread_mutex_lock(&mutex);
		int idx = ashv_find_local_index(shmid);
		if (idx != -1) {
			if (write(sendsock, &shmem[idx].key, sizeof(key_t)) != sizeof(key_t)) {
				DBG("%s: ERROR: write failed: %s", __PRETTY_FUNCTION__, strerror(errno));
			}
			if (ancil_send_fd(sendsock, shmem[idx].descriptor) != 0) {
				DBG("%s: ERROR: ancil_send_fd() failed: %s", __PRETTY_FUNCTION__, strerror(errno));
			}
		} else {
			DBG("%s: ERROR: cannot find shmid 0x%x", __PRETTY_FUNCTION__, shmid);
		}
		pthread_mutex_unlock(&mutex);
		close(sendsock);
		len = sizeof(addr);
	}
	DBG ("%s: ERROR: listen() failed, thread stopped", __PRETTY_FUNCTION__);
	return NULL;
}

static void android_shmem_delete(int idx)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	if (shmem[idx].descriptor) close(shmem[idx].descriptor);
	shmem_amount--;
	memmove(&shmem[idx], &shmem[idx+1], (shmem_amount - idx) * sizeof(shmem_t));
}

static int ashv_read_remote_segment(int shmid)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	sprintf(&addr.sun_path[1], ANDROID_SHMEM_SOCKNAME, ashv_socket_id_from_shmid(shmid));
	int addrlen = sizeof(addr.sun_family) + strlen(&addr.sun_path[1]) + 1;

	int recvsock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (recvsock == -1) {
		DBG ("%s: cannot create UNIX socket: %s", __PRETTY_FUNCTION__, strerror(errno));
		return -1;
	}
	if (connect(recvsock, (struct sockaddr*) &addr, addrlen) != 0) {
		DBG("%s: Cannot connect to UNIX socket %s: %s, len %d", __PRETTY_FUNCTION__, addr.sun_path + 1, strerror(errno), addrlen);
		close(recvsock);
		return -1;
	}

	if (send(recvsock, &shmid, sizeof(shmid), 0) != sizeof(shmid)) {
		DBG ("%s: send() failed on socket %s: %s", __PRETTY_FUNCTION__, addr.sun_path + 1, strerror(errno));
		close(recvsock);
		return -1;
	}

	key_t key;
	if (read(recvsock, &key, sizeof(key_t)) != sizeof(key_t)) {
		DBG("%s: ERROR: failed read", __PRETTY_FUNCTION__);
		close(recvsock);
		return -1;
	}

	int descriptor = ancil_recv_fd(recvsock);
	if (descriptor < 0) {
		DBG("%s: ERROR: ancil_recv_fd() failed on socket %s: %s", __PRETTY_FUNCTION__, addr.sun_path + 1, strerror(errno));
		close(recvsock);
		return -1;
	}
	close(recvsock);

	int size = ashmem_get_size_region(descriptor);
	if (size == 0 || size == -1) {
		DBG ("%s: ERROR: ashmem_get_size_region() returned %d on socket %s: %s", __PRETTY_FUNCTION__, size, addr.sun_path + 1, strerror(errno));
		return -1;
	}

	int idx = shmem_amount;
	shmem_amount ++;
	shmem = realloc(shmem, shmem_amount * sizeof(shmem_t));
	shmem[idx].id = shmid;
	shmem[idx].descriptor = descriptor;
	shmem[idx].size = size;
	shmem[idx].addr = NULL;
	shmem[idx].markedForDeletion = false;
	shmem[idx].key = key;
	return idx;
}

/* Get shared memory area identifier. */
int shmget(key_t key, size_t size, int flags)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	(void) flags;

	ashv_check_pid();

	// Counter wrapping around at 15 bits.
	static size_t shmem_counter = 0;

	if (!ashv_listening_thread_id) {
		int sock = socket(AF_UNIX, SOCK_STREAM, 0);
		if (!sock) {
			DBG ("%s: cannot create UNIX socket: %s", __PRETTY_FUNCTION__, strerror(errno));
			errno = EINVAL;
			return -1;
		}
		int i;
		for (i = 0; i < 4096; i++) {
			struct sockaddr_un addr;
			int len;
			memset (&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			ashv_local_socket_id = (getpid() + i) & 0xffff;
			sprintf(&addr.sun_path[1], ANDROID_SHMEM_SOCKNAME, ashv_local_socket_id);
			len = sizeof(addr.sun_family) + strlen(&addr.sun_path[1]) + 1;
			if (bind(sock, (struct sockaddr *)&addr, len) != 0) continue;
			DBG("%s: bound UNIX socket %s in pid=%d", __PRETTY_FUNCTION__, addr.sun_path + 1, getpid());
			break;
		}
		if (i == 4096) {
			DBG("%s: cannot bind UNIX socket, bailing out", __PRETTY_FUNCTION__);
			ashv_local_socket_id = 0;
			errno = ENOMEM;
			return -1;
		}
		if (listen(sock, 4) != 0) {
			DBG("%s: listen failed", __PRETTY_FUNCTION__);
			errno = ENOMEM;
			return -1;
		}
		int* socket_arg = malloc(sizeof(int));
		*socket_arg = sock;
		pthread_create(&ashv_listening_thread_id, NULL, &ashv_thread_function, socket_arg);
	}

	int shmid = -1;

	pthread_mutex_lock(&mutex);
	char symlink_path[256];
	if (key != IPC_PRIVATE) {
		// (1) Check if symlink exists telling us where to connect.
		// (2) If so, try to connect and open.
		// (3) If connected and opened, done. If connection refused
		//     take ownership of the key and create the symlink.
		// (4) If no symlink, create it.
		sprintf(symlink_path, ASHV_KEY_SYMLINK_PATH, key);
		char path_buffer[256];
		char num_buffer[64];
		while (true) {
			int path_length = readlink(symlink_path, path_buffer, sizeof(path_buffer) - 1);
			if (path_length != -1) {
				path_buffer[path_length] = '\0';
				int shmid = atoi(path_buffer);
				if (shmid != 0) {
					int idx = ashv_find_local_index(shmid);

					if (idx == -1) {
						idx = ashv_read_remote_segment(shmid);
					}

					if (idx != -1) {
						pthread_mutex_unlock(&mutex);
						return shmem[idx].id;
					}
				}
				// TODO: Not sure we should try to remove previous owner if e.g.
				// there was a tempporary failture to get a soket. Need to
				// distinguish between why ashv_read_remote_segment failed.
				unlink(symlink_path);
			}
			// Take ownership.
			// TODO: HAndle error (out of resouces, no infinite loop)
			if (shmid == -1) {
				shmem_counter = (shmem_counter + 1) & 0x7fff;
				shmid = ashv_shmid_from_counter(shmem_counter);
				sprintf(num_buffer, "%d", shmid);
			}
			if (symlink(num_buffer, symlink_path) == 0) break;
		}
	}


	int idx = shmem_amount;
	char buf[256];
	sprintf(buf, ANDROID_SHMEM_SOCKNAME "-%d", ashv_local_socket_id, idx);

	shmem_amount++;
	if (shmid == -1) {
		shmem_counter = (shmem_counter + 1) & 0x7fff;
		shmid = ashv_shmid_from_counter(shmem_counter);
	}

	long page_size = sysconf(_SC_PAGESIZE);
	shmem = realloc(shmem, shmem_amount * sizeof(shmem_t));
	size = ROUND_UP(size, page_size);
	shmem[idx].size = size;
	shmem[idx].descriptor = ashmem_create_region(buf, size);
	shmem[idx].addr = NULL;
	shmem[idx].id = shmid;
	shmem[idx].markedForDeletion = false;
	shmem[idx].key = key;

	if (shmem[idx].descriptor < 0) {
		DBG("%s: ashmem_create_region() failed for size %zu: %s", __PRETTY_FUNCTION__, size, strerror(errno));
		shmem_amount --;
		shmem = realloc(shmem, shmem_amount * sizeof(shmem_t));
		pthread_mutex_unlock (&mutex);
		return -1;
	}
	DBG("%s: ID %d shmid %x FD %d size %zu", __PRETTY_FUNCTION__, idx, shmid, shmem[idx].descriptor, shmem[idx].size);
	/*
	status = ashmem_set_prot_region (shmem[idx].descriptor, 0666);
	if (status < 0) {
		DBG ("%s: ashmem_set_prot_region() failed for size %zu: %s %d", __PRETTY_FUNCTION__, size, strerror(status), status);
		shmem_amount --;
		shmem = realloc (shmem, shmem_amount * sizeof(shmem_t));
		pthread_mutex_unlock (&mutex);
		return -1;
	}
	*/
	/*
	status = ashmem_pin_region (shmem[idx].descriptor, 0, shmem[idx].size);
	if (status < 0) {
		DBG ("%s: ashmem_pin_region() failed for size %zu: %s %d", __PRETTY_FUNCTION__, size, strerror(status), status);
		shmem_amount --;
		shmem = realloc (shmem, shmem_amount * sizeof(shmem_t));
		pthread_mutex_unlock (&mutex);
		return -1;
	}
	*/
	pthread_mutex_unlock(&mutex);

	return shmid;
}

/* Attach shared memory segment. */
void* shmat(int shmid, void const* shmaddr, int shmflg)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	ashv_check_pid();

	int socket_id = ashv_socket_id_from_shmid(shmid);
	void *addr;

	pthread_mutex_lock(&mutex);

	int idx = ashv_find_local_index(shmid);
	if (idx == -1 && socket_id != ashv_local_socket_id) {
		idx = ashv_read_remote_segment(shmid);
	}

	if (idx == -1) {
		DBG ("%s: shmid %x does not exist", __PRETTY_FUNCTION__, shmid);
		pthread_mutex_unlock(&mutex);
		errno = EINVAL;
		return (void*) -1;
	}

	if (shmem[idx].addr == NULL) {
		shmem[idx].addr = mmap((void*) shmaddr, shmem[idx].size, PROT_READ | (shmflg == 0 ? PROT_WRITE : 0), MAP_SHARED, shmem[idx].descriptor, 0);
		if (shmem[idx].addr == MAP_FAILED) {
			DBG ("%s: mmap() failed for ID %x FD %d: %s", __PRETTY_FUNCTION__, idx, shmem[idx].descriptor, strerror(errno));
			shmem[idx].addr = NULL;
		}
	}
	addr = shmem[idx].addr;
	DBG ("%s: mapped addr %p for FD %d ID %d", __PRETTY_FUNCTION__, addr, shmem[idx].descriptor, idx);
	pthread_mutex_unlock (&mutex);

	return addr ? addr : (void *)-1;
}

/* Detach shared memory segment. */
int shmdt(void const* shmaddr)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	ashv_check_pid();

	pthread_mutex_lock(&mutex);
	for (size_t i = 0; i < shmem_amount; i++) {
		if (shmem[i].addr == shmaddr) {
			if (munmap(shmem[i].addr, shmem[i].size) != 0) {
				DBG("%s: munmap %p failed", __PRETTY_FUNCTION__, shmaddr);
			}
			shmem[i].addr = NULL;
			DBG("%s: unmapped addr %p for FD %d ID %zu shmid %x", __PRETTY_FUNCTION__, shmaddr, shmem[i].descriptor, i, shmem[i].id);
			if (shmem[i].markedForDeletion || ashv_socket_id_from_shmid(shmem[i].id) != ashv_local_socket_id) {
				DBG ("%s: deleting shmid %x", __PRETTY_FUNCTION__, shmem[i].id);
				android_shmem_delete(i);
			}
			pthread_mutex_unlock(&mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&mutex);

	DBG("%s: invalid address %p", __PRETTY_FUNCTION__, shmaddr);
	/* Could be a remove segment, do not report an error for that. */
	return 0;
}

/* Shared memory control operation. */
int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
	DBG("%s" , __PRETTY_FUNCTION__);

	ashv_check_pid();

	if (cmd == IPC_RMID) {
		DBG("%s: IPC_RMID for shmid=%x", __PRETTY_FUNCTION__, shmid);
		pthread_mutex_lock(&mutex);
		int idx = ashv_find_local_index(shmid);
		if (idx == -1) {
			DBG("%s: shmid=%x does not exist locally", __PRETTY_FUNCTION__, shmid);
			/* We do not rm non-local regions, but do not report an error for that. */
			pthread_mutex_unlock(&mutex);
			return 0;
		}

		if (shmem[idx].addr) {
			// shmctl(2): The segment will actually be destroyed only
			// after the last process detaches it (i.e., when the shm_nattch
			// member of the associated structure shmid_ds is zero.
			shmem[idx].markedForDeletion = true;
		} else {
			android_shmem_delete(idx);
		}
		pthread_mutex_unlock(&mutex);
		return 0;
	} else if (cmd == IPC_STAT) {
		if (!buf) {
			DBG ("%s: ERROR: buf == NULL for shmid %x", __PRETTY_FUNCTION__, shmid);
			errno = EINVAL;
			return -1;
		}

		pthread_mutex_lock(&mutex);
		int idx = ashv_find_local_index(shmid);
		if (idx == -1) {
			DBG ("%s: ERROR: shmid %x does not exist", __PRETTY_FUNCTION__, shmid);
			pthread_mutex_unlock (&mutex);
			errno = EINVAL;
			return -1;
		}
		/* Report max permissive mode */
		memset(buf, 0, sizeof(struct shmid_ds));
		buf->shm_segsz = shmem[idx].size;
		buf->shm_nattch = 1;
		buf->shm_perm.__key = shmem[idx].key;
		buf->shm_perm.uid = geteuid();
		buf->shm_perm.gid = getegid();
		buf->shm_perm.cuid = geteuid();
		buf->shm_perm.cgid = getegid();
		buf->shm_perm.mode = 0666;
		buf->shm_perm.__seq = 1;

		pthread_mutex_unlock (&mutex);
		return 0;
	}

	DBG("%s: cmd %d not implemented yet!", __PRETTY_FUNCTION__, cmd);
	errno = EINVAL;
	return -1;
}

/* Make alias for use with e.g. dlopen() */
#undef shmctl
int shmctl(int shmid, int cmd, struct shmid_ds *buf) __attribute__((alias("libandroid_shmctl")));
#undef shmget
int shmget(key_t key, size_t size, int flags) __attribute__((alias("libandroid_shmget")));
#undef shmat
void* shmat(int shmid, void const* shmaddr, int shmflg) __attribute__((alias("libandroid_shmat")));
#undef shmdt
int shmdt(void const* shmaddr) __attribute__((alias("libandroid_shmdt")));
