# Investigation Report: Antigravity CLI TCMalloc Failure on Android/Chroot

## 1. The Initial Failure (Official Installer)
When running the official installation command:
```bash
curl -fsSL https://antigravity.google/cli/install.sh | bash
```
The installation failed immediately with the following error:
```text
FATAL ERROR: Out of memory trying to allocate internal tcmalloc data
MmapAligned() failed - unable to allocate with tag (hint=0x2f4c00000000, size=1073741824)
```

### Root Cause
The Antigravity CLI is compiled with **TCMalloc**, which assumes a **48-bit Virtual Address (VA) space** (standard for modern Linux servers). However, most **ARM64 Android kernels** (and chroots like Ubuntu running on them) are limited to a **39-bit VA space**. TCMalloc tried to reserve memory at an address that technically "does not exist" on this kernel, causing a hard crash.

---

## 2. Failure of the Standard Binary Patch
We attempted to use a community-maintained "Termux Patch" which uses Python to byte-edit the binary and rewrite the `ubfx` and `mmap` instructions.

### Why it failed in the Chroot
While the binary was patched, the **C Bootstrapper** (the `agy` helper) was hardcoded for native Termux paths:
- It looked for `/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1`.
- It looked for Termux-specific SSL certificate paths.
In the Ubuntu chroot, these paths do not exist, causing "No such file or directory" errors.

---

## 3. The Successful Solution: "Mmap Interposition"
The final working solution involved a technique called **Dynamic Library Interposition**.

### The Method
1. **Poking the Kernel:** We wrote a probe (`mmap_test.c`) to confirm the kernel's exact limit is **2^38**.
2. **The Fixer Library:** We created a small C library (`libmmap_va39_fix.c`) that intercepts all `mmap` calls made by the CLI.
   - If the CLI asks for a "hint" address above the 39-bit boundary, our library **clears the hint** (sets it to `NULL`).
   - This allows the Linux kernel to pick a safe, lower address for the allocation instead of crashing.
3. **Smart Bootstrapper:** We updated the `agy` bootstrapper to:
   - **Auto-Detect Environment:** Check if it's in Termux or a standard Linux chroot (Ubuntu).
   - **Set Dynamic Paths:** Use `/lib/ld-linux-aarch64.so.1` and `/etc/ssl/certs/ca-certificates.crt` when in Ubuntu.
   - **Auto-Preload:** Automatically inject the `mmap` fixer library using the `--preload` flag of the glibc loader.

### Why it Succeeded
This method is more robust than binary patching because it handles memory requests **at runtime**. It doesn't matter what TCMalloc is hardcoded to do; our "interposer" sits between the program and the kernel, correcting every dangerous memory request before the kernel even sees it.

---

## 4. Integration for Future Releases
I have integrated this fix into the `antigravity-cli-termux` project structure:
- **New Source:** `lib/mmap_va39_fix.c` contains the runtime memory fix.
- **Updated Build:** `build.sh` now compiles this library automatically.
- **Updated Logic:** `lib/agy_helper.c` now detects the environment and applies the fix seamlessly.

This ensures that any future updates to the CLI will automatically be compatible with both native Termux and Ubuntu chroot environments.
