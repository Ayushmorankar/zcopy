#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include "zcopy_ioctl.h"

#define DEVICE_NAME "zcopy"
#define DEFAULT_BUFFER_SIZE 1024
#define ZCPY_DEVS 4

struct zcpy_dev{
    char *kernel_buffer;
    size_t buffer_size;
    struct rw_semaphore rw_sem; /* mutual exclusion read-write semaphore */
    struct cdev cdev; /* Char device structure */
};

static struct zcpy_dev zcpy_devices[ZCPY_DEVS] = {0};
static dev_t dev_num;
static struct class *zcpy_class;

// Called when userspace does open()
static int zcpy_open(struct inode *inode, struct file *filp) {
    struct zcpy_dev *dev = container_of(inode->i_cdev, struct zcpy_dev, cdev);
    filp->private_data = dev;
    pr_info("zcopy: opened\n");
    return 0;
}

// Called when userspace does read()
static ssize_t zcpy_read(struct file *filp, char __user *user_buf, size_t count, loff_t *offset) {

    struct zcpy_dev *devp = filp->private_data;
    size_t buffer_size = devp->buffer_size;

    // CRITICAL SECTION: START

    // Accquire the read lock
    // Here the driver will return[in case signal like ctrl+C is recieved] if its put to sleep maybe coz its waiting for the write lock to be released
    if (down_read_killable(&devp->rw_sem))
        return -ERESTARTSYS;

    pr_info("zcopy[read]: read requested for %zu bytes with offset %lld\n", count, *offset);

    // check if the buffer is already full
    if(*offset >= buffer_size){
        pr_info("zcopy[read]: read offset %lld is out of bounds\n", *offset);
        // release read lock
        up_read(&(devp->rw_sem));
        return 0;
    }

    char* kernel_buffer_from = devp->kernel_buffer;
    kernel_buffer_from += *offset;

    size_t bytes_to_cpy_in_us = (count + *offset)<buffer_size ? count : buffer_size - *offset;

    pr_info("zcopy[read]: copying %zu bytes to user\n", bytes_to_cpy_in_us);

    unsigned long bytes_failed_to_cpy = copy_to_user(user_buf, kernel_buffer_from, bytes_to_cpy_in_us);

    int delta = bytes_to_cpy_in_us - bytes_failed_to_cpy;

    pr_info("zcopy[read]: copied %d bytes to user\n", delta);

    if(bytes_failed_to_cpy != 0) {
        pr_warn("zcopy[read]: failed to copy %lu bytes to user\n", bytes_failed_to_cpy);
        up_read(&devp->rw_sem);
        return delta;
    }

    *offset += bytes_to_cpy_in_us;

    up_read(&devp->rw_sem);
    // CRITICAL SECTION: END

    return delta;
}

// Called when userspace does write()
static ssize_t zcpy_write(struct file *filp, const char __user *user_buf, size_t count, loff_t *offset) {

    struct zcpy_dev *devp = filp->private_data;
    size_t buffer_size = devp->buffer_size;

    // CRITICAL SECTION: START

    // Accquire write lock
    if(down_write_killable(&devp->rw_sem)) {
        return -ERESTARTSYS;
    }

    pr_info("zcopy[write]: write requested for %zu bytes with offset %lld\n", count, *offset);

    // check if the buffer is already full
    if(*offset >= buffer_size){
        pr_info("zcopy[write]: write offset %lld is out of bounds\n", *offset);
        up_write(&(devp->rw_sem));
        // release write lock
        return -ENOSPC;
    }

    // small writes
    size_t bytes_to_write = min_t(size_t, count, buffer_size-*offset);

    char* kernel_buffer_to = devp->kernel_buffer;
    kernel_buffer_to += *offset;

    pr_info("zcopy[write]: copying %zu bytes from user\n", bytes_to_write);

    unsigned long bytes_failed_to_cpy = copy_from_user((void*)kernel_buffer_to, user_buf, bytes_to_write);

    // If copy_from_user fails, it returns the number of bytes that couldn't be copied.
    // So if it returns non-zero, it means some bytes failed to copy.
    if(bytes_failed_to_cpy != 0) {
        pr_warn("zcopy: failed to receive %lu bytes from user\n", bytes_failed_to_cpy);
        // release write lock
        up_write(&(devp->rw_sem));
        return -EFAULT;
    }

    *offset += bytes_to_write;

    // Release write lock
    up_write(&(devp->rw_sem));


    // CRITICAL SECTION: END
    return bytes_to_write;
}

static void clear_buffer(struct zcpy_dev *devp){
    memset(devp->kernel_buffer, 0, devp->buffer_size);
}

static int init_buffer(struct zcpy_dev *devp, size_t buffer_size){
    char *buffer = kmalloc(buffer_size, GFP_KERNEL);
    if (buffer == NULL) {
        return -ENOMEM;
    }
    if(devp->kernel_buffer!=NULL){
        memcpy(buffer, devp->kernel_buffer, min(devp->buffer_size, buffer_size));
    }
    devp->kernel_buffer = buffer;
    return 1;
}

static int free_buffer(struct zcpy_dev *devp){
    if (devp->kernel_buffer) {
        kfree(devp->kernel_buffer);
        devp->kernel_buffer = NULL;  // avoid dangling pointer
    }
    return 1;
}

static long zcpy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){

    /*
    * wrong cmds: return ENOTTY (inappropriate ioctl)
    */
    if (_IOC_TYPE(cmd) != ZCPY_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > ZCPY_IOC_MAXNR) return -ENOTTY;

    struct zcpy_dev *devp = filp->private_data;

    switch (cmd) {

        case ZCPY_IOC_CLEAR: {
            pr_debug("zcopy[ioctl]: ZCPY_IOC_CLEAR requested\n");

            // Acquire exclusive write lock
            if(down_write_killable(&devp->rw_sem)) {
                pr_warn("zcopy[ioctl]: CLEAR interrupted by signal\n");
                return -ERESTARTSYS;
            }

            clear_buffer(devp);
            pr_info("zcopy[ioctl]: buffer cleared successfully\n");

            // Release write lock
            up_write(&(devp->rw_sem));
            break;
        }

        case ZCPY_IOC_GSIZE: {
            pr_debug("zcopy[ioctl]: ZCPY_IOC_GSIZE requested\n");

            // Acquire exclusive read lock
            if (down_read_killable(&devp->rw_sem)){
                return -ERESTARTSYS;
            }

            size_t buffer_size = devp->buffer_size;
            if (put_user(buffer_size, (unsigned long __user *)arg)){
                pr_warn("zcopy[ioctl]: GSIZE failed to copy %zu to user-space\n", buffer_size);
                up_read(&(devp->rw_sem));
                return -EFAULT;
            }

            pr_info("zcopy[ioctl]: GSIZE returned %zu bytes\n", buffer_size);

            // Release read lock
            up_read(&(devp->rw_sem));
            break;
        }

        case ZCPY_IOC_SSIZE: {
            pr_debug("zcopy[ioctl]: ZCPY_IOC_SSIZE requested\n");

            // Acquire exclusive write lock
            if(down_write_killable(&devp->rw_sem)) {
                return -ERESTARTSYS;
            }

            size_t new_buffer_size;
            if(get_user(new_buffer_size, (unsigned long __user *)arg)){
                pr_warn("zcopy[ioctl]: SSIZE failed to read new size from user-space\n");
                up_write(&(devp->rw_sem));
                return -EFAULT;
            }

            pr_info("zcopy[ioctl]: SSIZE attempting to resize from %zu to %zu bytes\n",
                    devp->buffer_size, new_buffer_size);

            char *old_buffer = devp->kernel_buffer;
            if(init_buffer(devp, new_buffer_size) <= 0){
                pr_err("zcopy[ioctl]: SSIZE memory allocation failed for %zu bytes\n", new_buffer_size);
                devp->kernel_buffer = old_buffer;
                up_write(&(devp->rw_sem));
                return -ENOMEM;
            }

            devp->buffer_size = new_buffer_size;
            kfree(old_buffer);

            pr_info("zcopy[ioctl]: SSIZE resize successful\n");

            // Release write lock
            up_write(&(devp->rw_sem));
            break;
        }

        case ZCPY_IOC_GCURSOR: {
            pr_debug("zcopy[ioctl]: ZCPY_IOC_GCURSOR requested\n");

            // Acquire exclusive read lock
            if(down_read_killable(&devp->rw_sem)) {
                return -ERESTARTSYS;
            }

            if (put_user(filp->f_pos, (loff_t __user *)arg)){
                pr_warn("zcopy[ioctl]: GCURSOR failed to copy offset to user-space\n");
                up_read(&(devp->rw_sem));
                return -EFAULT;
            }

            pr_info("zcopy[ioctl]: GCURSOR returned offset %lld\n", filp->f_pos);

            // Release read lock
            up_read(&(devp->rw_sem));
            break;
        }


        case ZCPY_IOC_SRESETCURSOR: {
            pr_debug("zcopy[ioctl]: ZCPY_IOC_SRESETCURSOR requested\n");

            // Acquire exclusive write lock
            if (down_write_killable(&devp->rw_sem)) {
                return -ERESTARTSYS;
            }

            filp->f_pos = 0;
            pr_info("zcopy[ioctl]: cursor reset to 0\n");

            // Release write lock
            up_write(&(devp->rw_sem));
            break;
        }

        default:
            pr_warn("zcopy[ioctl]: unhandled command ordinal %u\n", _IOC_NR(cmd));
            return -ENOTTY;
    }

    return 0;
}

static struct file_operations fops = {
    .open  = zcpy_open,
    .read  = zcpy_read,
    .write = zcpy_write,
    .unlocked_ioctl = zcpy_ioctl,
};

static int __init zcpy_init(void) {

    int result = alloc_chrdev_region(&dev_num, 0, ZCPY_DEVS, DEVICE_NAME);

    if(result){
        // If result is a -ve number then allocation failed
        pr_err("zcopy: init failed with code [%d]\n", result);
        return result;
    }
    else{
        // result = 0
        // success!
        int major_number = MAJOR(dev_num);
        pr_info("zcopy: dynamically allocated major number %d\n", major_number);

        // Create the sysfs class
        zcpy_class = class_create("zcopy_class");
        if (IS_ERR(zcpy_class)) {
            pr_err("zcopy: failed to create class\n");
            unregister_chrdev_region(dev_num, ZCPY_DEVS);
            return PTR_ERR(zcpy_class);
        }

        // init devices
        for(int i = 0; i<ZCPY_DEVS; i++){

            zcpy_devices[i].buffer_size = DEFAULT_BUFFER_SIZE;

            // init rw_semaphore
            init_rwsem(&zcpy_devices[i].rw_sem);

            // init buffer
            init_buffer(&zcpy_devices[i], DEFAULT_BUFFER_SIZE);

            // init cdev
            cdev_init(&zcpy_devices[i].cdev, &fops);
            zcpy_devices[i].cdev.owner = THIS_MODULE;

            // Get the dev_t for ith device
            dev_t dev_num_i = MKDEV(major_number, i);

            // add cdev
            int err = cdev_add(&zcpy_devices[i].cdev, dev_num_i, 1);

            // cdev_add fails
            if(err<0){
                pr_err("zcopy: cdev_add failed\n");
                // remove all the previous registered devices
                for(int j = i-1; j>=0; j--){
                    free_buffer(&zcpy_devices[j]);
                    device_destroy(zcpy_class, MKDEV(major_number, j));
                    cdev_del(&zcpy_devices[j].cdev);
                }
                class_destroy(zcpy_class);
                unregister_chrdev_region(dev_num, ZCPY_DEVS);
                return err;
            }
            device_create(zcpy_class, NULL, dev_num_i, NULL, "zcopy%d", i);
        }
    }
    return 0;
}

static void __exit zcpy_exit(void) {
    for(int i = 0; i<ZCPY_DEVS; i++){
        free_buffer(&zcpy_devices[i]);
        device_destroy(zcpy_class, MKDEV(MAJOR(dev_num), i));
        cdev_del(&zcpy_devices[i].cdev);
    }
    class_destroy(zcpy_class);
    unregister_chrdev_region(dev_num, ZCPY_DEVS);
    pr_info("zcopy: unregistered\n");
}

module_init(zcpy_init);
module_exit(zcpy_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ayush Morankar");
