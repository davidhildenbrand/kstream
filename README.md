# kstream
kstream - STREAM benchmark inside a Linux kernel module

kstream allocates individual 4 MiB chunks of memory (maximum granularity of the buddy page allocator) in increasing address order and run a modified STREAM benchmark on each individual chunk; it measure the memory bandwidth a) without flushing caches b) flushing caches before every memory access.

Results for each memory chunk will currently be printed using printk(), flooding the system log.

*Note: x86 only and pretty much untested. The results might be quite unreliable. Useful for identifying problematic physical memory ranges.*

## Instructions

Your kernel has to be compiled without enforced module signatures (CONFIG_MODULE_SIG_FORCE) and secureboot might have to be disabled.

When compiling for a distribution kernel, you'll have to install kernel-devel packages.

1. Compile the module
 
`$ make`

2. Load the module to start the benchmark

`$ insmod kstream.ko`

3. Wait until the benchmark finished, or stop the benchmark any time

`$ rmmod kstream`
