#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdint.h>

// hacks in attempt to define android types
#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdint.h>

typedef int key_t;
typedef unsigned short shmatt_t;
struct shmid64_ds {
    struct ipc_perm shm_perm;    /* operation perms */
    size_t          shm_segsz;   /* size of segment (bytes) */
    time_t          shm_atime;   /* last attach time */
    time_t          shm_dtime;   /* last detach time */
    time_t          shm_ctime;   /* last change time */
    pid_t           shm_cpid;    /* pid of creator */
    pid_t           shm_lpid;    /* pid of last operator */
    shmatt_t        shm_nattch;  /* no. of current attaches */
    /* additional fields may be required depending on your system */
};
long page_size = sysconf(_SC_PAGESIZE);
// end hacks
__BEGIN_DECLS

#ifndef shmid_ds
# define shmid_ds shmid64_ds
#endif

/* Shared memory control operations. */
#undef shmctl
#define shmctl libandroid_shmctl
extern int shmctl(int shmid, int cmd, struct shmid_ds* buf);

/* Get shared memory area identifier. */
#undef shmget
#define shmget libandroid_shmget
extern int shmget(key_t key, size_t size, int shmflg);

/* Attach shared memory segment. */
#undef shmat
#define shmat libandroid_shmat
extern void *shmat(int shmid, void const* shmaddr, int shmflg);

/* Detach shared memory segment. */
#undef shmdt
#define shmdt libandroid_shmdt
extern int shmdt(void const* shmaddr);

__END_DECLS

#endif
typedef int key_t;
typedef unsigned short shmatt_t;
struct shmid64_ds {
    struct ipc_perm shm_perm;    /* operation perms */
    size_t          shm_segsz;   /* size of segment (bytes) */
    time_t          shm_atime;   /* last attach time */
    time_t          shm_dtime;   /* last detach time */
    time_t          shm_ctime;   /* last change time */
    pid_t           shm_cpid;    /* pid of creator */
    pid_t           shm_lpid;    /* pid of last operator */
    shmatt_t        shm_nattch;  /* no. of current attaches */
    /* additional fields may be required depending on your system */
};
// end hacks
__BEGIN_DECLS

#ifndef shmid_ds
# define shmid_ds shmid64_ds
#endif

/* Shared memory control operations. */
#undef shmctl
#define shmctl libandroid_shmctl
extern int shmctl(int shmid, int cmd, struct shmid_ds* buf);

/* Get shared memory area identifier. */
#undef shmget
#define shmget libandroid_shmget
extern int shmget(key_t key, size_t size, int shmflg);

/* Attach shared memory segment. */
#undef shmat
#define shmat libandroid_shmat
extern void *shmat(int shmid, void const* shmaddr, int shmflg);

/* Detach shared memory segment. */
#undef shmdt
#define shmdt libandroid_shmdt
extern int shmdt(void const* shmaddr);

__END_DECLS

#endif
