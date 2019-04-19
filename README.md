### oom_ext

### kernel module for OOM-state mitigation

## The problem

OOM (out-of-memory) state is triggered under memory shortage conditions when 
kernel cannot free any additional memory pages to satisfy page request, and 
it is not likely that any pages will be reclaimed in the nearest time. Once in
OOM state, a process is selected to be killed, alongside with a set of other
processes sharing the same mm_struct. These processes will be send TERM/KILL
signals, while the other processes(threads) will be locked.
Due to a set of algorythm's drawbacks, it is a frequent situation that the 
processes will never be able to exit, and machine would hang indefinitely until
hard-rebooted.

## Solution

When loaded, oom_ext reserves a portion of non-swappable memory as emergency
buffer. Once OOM state is detected, the buffer is freed, effectively bringing
the system out of OOM state an thus unlocking all the processes. The buffer can
doesn't need to any huge, it just have to be large enough to win a few seconds
of 'unlocked' operation for OOM-killer target processes to actually shut down.

## Installation

Execute `make` inside code directory to build the module. After that, you will
be able to try the module immediately with `insmod oom_ext.ko`. To install the
module, execute `make install`. This will install the module under kernel
modules directory, and it will be available for loading with `modprobe oom_ext`.
Please note that installation will not set this module to autoload on boot: I 
prefer to leave autoloading (or not autoloading :) part to end-user).

## Configuration

The module adds a few sysctl's with which it can be configured:

| Parameter | Description | Default |
|----------:|:------------|:--------|
| **vm.oom_ext.gracetime** | time(sec) in OOM state before kernel panic (usable to reboot), 0 to disable panic | `0` |
| **vm.oom_ext.resettime** | time(sec) out of OOM before recreating emergency buffer | `300` |
| **vm.oom_ext.bufsize** | emergency buffer in MB, 0 to disable  | `32` |
| **vm.oom_ext.crashflag** | set crash flag file on fs, will be removed if oom ended, 0 to disable  | `1` |
| **vm.oom_ext.crashflag_name** | pathname for crash flag file  | `/oomflag` |

