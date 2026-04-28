# zcopy (Stage 1) — Kernel Buffer Character Device

A foundation-level Linux character device driver that exposes a 1024-byte kernel memory buffer to userspace. This project demonstrates core kernel module concepts including dynamic registration, file operations, and concurrency control.

---

## Features

**Dynamic Registration** — Automatically allocates major/minor numbers and registers devices (`/dev/zcopy0` through `/dev/zcopy3`) using `udev` and sysfs.

**POSIX Compliance** — Implements safe "short reads" and "short writes" (`copy_to_user` / `copy_from_user`), making the buffer fully compatible with standard command-line tools like `cat` and `echo`.

**Concurrency Control** — Uses Reader-Writer Semaphores (`rw_semaphore`) with killable/interruptible states to allow simultaneous readers while safely locking out concurrent writers.

---

## Usage

### 1. Build the module

```bash
make
```

### 2. Load the driver

```bash
sudo insmod zcopy.ko
```

### 3. Interact with the buffer

```bash
# Write to the buffer
echo "Hello from userspace" > /dev/zcopy0

# Read from the buffer
cat /dev/zcopy0
```

### 4. Unload the driver

```bash
sudo rmmod zcopy.ko
```
