#ifndef ZCOPY_IOCTL_H
#define ZCOPY_IOCTL_H

/*
* S means "Set" through a ptr,
* G means "Get": reply by setting through a pointer
*/
#define ZCPY_IOC_MAGIC 'Z'

//ioctl commands
#define ZCPY_IOC_CLEAR          _IO(ZCPY_IOC_MAGIC, 0)                      /* clear buffer */
#define ZCPY_IOC_GSIZE          _IOR(ZCPY_IOC_MAGIC, 1, unsigned long)      /* get buffer size */
#define ZCPY_IOC_SSIZE          _IOW(ZCPY_IOC_MAGIC, 2, unsigned long)      /* set buffer size */
#define ZCPY_IOC_GCURSOR        _IOR(ZCPY_IOC_MAGIC, 3, long long)          /* get offset */
#define ZCPY_IOC_SRESETCURSOR   _IO(ZCPY_IOC_MAGIC, 4)                      /* reset offset */

#define ZCPY_IOC_MAXNR 4

#endif
