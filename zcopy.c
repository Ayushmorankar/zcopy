#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "zcopy"
#define BUFFER_SIZE 1024
#define ZCPY_DEVS 4

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ayush Morankar");

struct zcpy_dev{
    char kernel_buffer[BUFFER_SIZE];
    struct rw_semaphore rw_sem; /* mutual exclusion read-write semaphore */
    struct cdev cdev; /* Char device structure */
};

static struct zcpy_dev zcpy_devices[ZCPY_DEVS];
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

    struct zcpy_dev *dev = filp->private_data;

    // CRITICAL SECTION: START

    // Accquire the read lock
    // Here the driver will return[in case signal like ctrl+C is recieved] if its put to sleep maybe coz its waiting for the write lock to be released
    if (down_read_killable(&dev->rw_sem))
        return -ERESTARTSYS;

    pr_info("zcopy[read]: read requested for %zu bytes with offset %lld\n", count, *offset);

    // check if the buffer is already full
    if(*offset >= BUFFER_SIZE){
        pr_info("zcopy[read]: read offset %lld is out of bounds\n", *offset);
        up_read(&(dev->rw_sem));
        // release write lock
        return 0;
    }

    char* kernel_buffer_from = dev->kernel_buffer;
    kernel_buffer_from += *offset;

    size_t bytes_to_cpy_in_us = (count + *offset)<BUFFER_SIZE ? count : BUFFER_SIZE-*offset;

    pr_info("zcopy[read]: copying %zu bytes to user\n", bytes_to_cpy_in_us);

    unsigned long bytes_failed_to_cpy = copy_to_user(user_buf, kernel_buffer_from, bytes_to_cpy_in_us);

    int delta = bytes_to_cpy_in_us - bytes_failed_to_cpy;

    pr_info("zcopy[read]: copied %d bytes to user\n", delta);

    if(bytes_failed_to_cpy != 0) {
        pr_warn("zcopy[read]: failed to copy %lu bytes to user\n", bytes_failed_to_cpy);
        up_read(&dev->rw_sem);
        return delta;
    }

    *offset += bytes_to_cpy_in_us;

    up_read(&dev->rw_sem);
    // CRITICAL SECTION: END

    return delta;
}

// Called when userspace does write()
static ssize_t zcpy_write(struct file *filp, const char __user *user_buf, size_t count, loff_t *offset) {

    struct zcpy_dev *dev = filp->private_data;

    // CRITICAL SECTION: START

    // Accquire write lock
    if(down_write_killable(&dev->rw_sem)) {
        return -ERESTARTSYS;
    }

    pr_info("zcopy[write]: write requested for %zu bytes with offset %lld\n", count, *offset);

    // check if the buffer is already full
    if(*offset >= BUFFER_SIZE){
        pr_info("zcopy[write]: write offset %lld is out of bounds\n", *offset);
        up_write(&(dev->rw_sem));
        // release write lock
        return -ENOSPC;
    }

    // small writes
    size_t bytes_to_write = min_t(size_t, count, BUFFER_SIZE-*offset);

    char* kernel_buffer_to = dev->kernel_buffer;
    kernel_buffer_to += *offset;

    pr_info("zcopy[write]: copying %zu bytes from user\n", bytes_to_write);

    unsigned long bytes_failed_to_cpy = copy_from_user((void*)kernel_buffer_to, user_buf, bytes_to_write);

    // If copy_from_user fails, it returns the number of bytes that couldn't be copied.
    // So if it returns non-zero, it means some bytes failed to copy.
    if(bytes_failed_to_cpy != 0) {
        pr_warn("zcopy: failed to receive %lu bytes from user\n", bytes_failed_to_cpy);
        // release write lock
        up_write(&(dev->rw_sem));
        return -EFAULT;
    }

    *offset += bytes_to_write;

    // Release write lock
    up_write(&(dev->rw_sem));


    // CRITICAL SECTION: END
    return bytes_to_write;
}

static struct file_operations fops = {
    .open  = zcpy_open,
    .read  = zcpy_read,
    .write = zcpy_write,
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
            // init rw_semaphore
            init_rwsem(&zcpy_devices[i].rw_sem);

            // Get the dev_t for ith device
            dev_t dev_num_i = MKDEV(major_number, i);


            cdev_init(&zcpy_devices[i].cdev, &fops);
            zcpy_devices[i].cdev.owner = THIS_MODULE;
            int err = cdev_add(&zcpy_devices[i].cdev, dev_num_i, 1);
            // cdev_add fails
            if(err<0){
                pr_err("zcopy: cdev_add failed\n");
                // remove all the previous registered devices
                for(int j = i-1; j>=0; j--){
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
        device_destroy(zcpy_class, MKDEV(MAJOR(dev_num), i));
        cdev_del(&zcpy_devices[i].cdev);
    }
    class_destroy(zcpy_class);
    unregister_chrdev_region(dev_num, ZCPY_DEVS);
    pr_info("zcopy: unregistered\n");
}

module_init(zcpy_init);
module_exit(zcpy_exit);
