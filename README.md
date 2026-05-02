# zcopy (Stage 2) — Kernel Buffer Character Device with ioctl Interface

A Linux character device driver that exposes a dynamically-sized kernel memory buffer to userspace. Stage 2 adds an `ioctl` control interface on top of the Stage 1 foundation, demonstrating hardware-style command/control patterns used in real GPU, audio, and DMA drivers.

---

## Features

**Dynamic Registration** — Automatically allocates major/minor numbers and registers devices (`/dev/zcopy0` through `/dev/zcopy3`) using `udev` and sysfs.

**POSIX Compliance** — Implements safe short reads and short writes (`copy_to_user` / `copy_from_user`), making the buffer fully compatible with standard command-line tools like `cat` and `echo`.

**Concurrency Control** — Uses Reader-Writer Semaphores (`rw_semaphore`) with killable/interruptible states to allow simultaneous readers while safely serializing writers.

**ioctl Control Interface** — Exposes a typed, magic-number-validated command interface for buffer management. Each command is encoded as a 32-bit number split into direction, size, magic, and ordinal bitfields — preventing accidental cross-device command mismatch.

---

## ioctl Commands

| Command | Description |
|---|---|
| `ZCPY_IOC_CLEAR` |  Zero out the buffer |
| `ZCPY_IOC_GSIZE` |  Get current buffer size |
| `ZCPY_IOC_SSIZE` |  Set (resize) the buffer |
| `ZCPY_IOC_GCURSOR` |  Get current read/write offset |
| `ZCPY_IOC_SRESETCURSOR` |  Reset offset to 0 |

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
echo "Hello from userspace" | sudo tee /dev/zcopy0

# Read from the buffer
sudo cat /dev/zcopy0
```

### 4. Use ioctl commands (see zcopy_ioctl.h for usage)
```bash
# Example userspace program using ioctl
gcc -o test test.c && sudo ./test
```

### 5. Unload the driver
```bash
sudo rmmod zcopy.ko
```

---
