// ashmem.h (minimal version extracted from AOSP)
#ifndef _LINUX_ASHMEM_H
#define _LINUX_ASHMEM_H

#include <linux/ioctl.h>
#include <stdint.h>

#define ASHMEM_NAME_LEN 256

#define ASHMEM_NAME_DEFAULT "dev/ashmem"

struct ashmem_pin {
    uint32_t offset;
    uint32_t length;
};

#define ASHMEM_SET_NAME      _IOW('a', 1, char[ASHMEM_NAME_LEN])
#define ASHMEM_GET_NAME      _IOR('a', 2, char[ASHMEM_NAME_LEN])
#define ASHMEM_SET_SIZE      _IOW('a', 3, size_t)
#define ASHMEM_GET_SIZE      _IO('a', 4)
#define ASHMEM_SET_PROT_MASK _IOW('a', 5, unsigned long)
#define ASHMEM_GET_PROT_MASK _IO('a', 6)
#define ASHMEM_PIN           _IOW('a', 7, struct ashmem_pin)
#define ASHMEM_UNPIN         _IOW('a', 8, struct ashmem_pin)
#define ASHMEM_GET_PIN_STATUS _IO('a', 9)
#define ASHMEM_PURGE_ALL_CACHES _IO('a', 10)

#endif // _LINUX_ASHMEM_H
