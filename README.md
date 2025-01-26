# Watchman

`watchman` is a Linux userspace tool that monitors memory locations of a running process using hardware watchpoints. It uses the `ptrace` system call to attach to the target process and sets hardware watchpoints to detect when specific memory addresses are written to. When a watchpoint is hit, `watchman` sends a `SIGUSR2` signal to the traced process, allowing the process to handle the event appropriately.

## Table of Contents

- [Features](#features)
- [Prerequisites](#prerequisites)
- [Building Watchman](#building-watchman)
- [Usage](#usage)
  - [Command-Line Arguments](#command-line-arguments)
  - [Examples](#examples)
- [Limitations](#limitations)
- [Sample Test Program (`watchman_test`)](#sample-test-program-watchman_test)
  - [Steps to Use `watchman_test` with `watchman`](#steps-to-use-watchman_test-with-watchman)
- [Notes](#notes)
- [License](#license)

## Features

- Monitor up to **four** memory addresses simultaneously (hardware limitation).
- Detect writes to specified memory addresses.
- Send `SIGUSR2` to the traced process upon watchpoint hits.
- Compatible with multithreaded applications (attaches to all threads).

## Prerequisites

- **Linux** operating system.
- **GCC** compiler (or any compatible C compiler).
- **Root permissions** (required for `ptrace` operations).

## Building Watchman

1. **Clone the Repository**

   ```bash
   git clone https://github.com/yourusername/watchman.git
   cd watchman
   ```

2. **Compile `watchman`**

   ```bash
   gcc -g -o watchman watchman.c
   ```

3. **(Optional) Compile `watchman_test`**

   A sample test program is provided to demonstrate `watchman`'s functionality.

   ```bash
   gcc -g -o watchman_test watchman_test.c -lpthread
   ```

## Usage

```bash
sudo ./watchman <pid> <address1> <length1> [<address2> <length2> ...]
```

- **Note:** `sudo` or root permissions are required to use `ptrace` and set hardware watchpoints.

### Command-Line Arguments

- `<pid>`: The process ID of the target process you want to monitor.
- `<addressX>`: The memory address to set a watchpoint on.
- `<lengthX>`: The size of the memory region to watch starting from `<addressX>`.
  - Supported lengths: **1**, **2**, **4** bytes.
  - Due to hardware limitations, **8-byte watchpoints are not supported**.
- Up to **four** pairs of `<address>` and `<length>` can be specified.

### Examples

1. **Monitoring a Single Memory Address**

   ```bash
   sudo ./watchman 12345 0x7ffdabc12340 4
   ```

   - Monitors the 4-byte region at address `0x7ffdabc12340` in process `12345`.

2. **Monitoring Multiple Memory Addresses**

   ```bash
   sudo ./watchman 12345 0x7ffdabc12340 4 0x7ffdabc12350 2
   ```

   - Monitors:
     - 4-byte region at `0x7ffdabc12340`.
     - 2-byte region at `0x7ffdabc12350`.

3. **Monitoring an 8-Byte Variable**

   Since 8-byte watchpoints are not supported, **split the 8-byte region into two 4-byte watchpoints**.

   ```bash
   sudo ./watchman 12345 0x7ffdabc12340 4 0x7ffdabc12344 4
   ```

   - Monitors the 8-byte region starting at `0x7ffdabc12340` by watching two consecutive 4-byte regions.

## Limitations

- **Hardware Constraints**: Only up to **four** hardware watchpoints are available (`DR0`-`DR3`).
- **Watchpoint Lengths**: Only lengths of **1**, **2**, or **4** bytes are supported.
- **Address Alignment**: The memory address must be aligned to the size of the watchpoint.
  - For a 4-byte watchpoint, the address must be divisible by 4.
- **Signal Handling**: The traced process must handle `SIGUSR2` appropriately.
- **Root Permissions**: Required to attach to other processes and manipulate debug registers.

## Sample Test Program (`watchman_test`)

A test program, `watchman_test.c`, is provided to demonstrate how `watchman` works. This program:

- Declares some global variables.
- Starts a thread that modifies these variables in a loop.
- Sets up a signal handler for `SIGUSR2` to react when `watchman` detects changes.

### Steps to Use `watchman_test` with `watchman`

1. **Compile `watchman_test`**

   ```bash
   gcc -g -o watchman_test watchman_test.c -lpthread
   ```

2. **Run `watchman_test`**

   ```bash
   ./watchman_test
   ```

   - The program will output its PID and the addresses of the variables.

   **Sample Output:**

   ```
   [watchman_test] Starting main
   [watchman_test] pid=12345
     g_varA=0x55c6c19b0010
     g_varB=0x55c6c19b0018
     g_varC=0x55c6c19b0020
   [watchman_test] Thread started.
   ```

3. **Run `watchman` in Another Terminal**

   - Use the PID and variable addresses displayed by `watchman_test`.

   ```bash
   sudo ./watchman 12345 0x55c6c19b0018 4 0x55c6c19b001c 4
   ```

   - This sets two 4-byte watchpoints to monitor an 8-byte variable starting at `0x55c6c19b0018`.

4. **Observe the Output**

   - **`watchman` Output:**

     ```
     [watchman] Setting 2 watchpoint(s):
       Watchpoint 0: addr=0x55c6c19b0018, size=4
       Watchpoint 1: addr=0x55c6c19b001c, size=4
     [watchman] Trying to attach to TID=12345
     [watchman] TID=12345 stopped, status=0x137f
     [watchman] TID=12345 options set
     [watchman] TID=12345: Set watchpoint 0 at addr=0x55c6c19b0018 with DR7=0x70001
     [watchman] TID=12345: Set watchpoint 1 at addr=0x55c6c19b001c with DR7=0x70005
     [watchman] TID=12345 continued
     [watchman] Starting main event loop...
     ```

   - **`watchman_test` Output Upon Watchpoint Hit:**

     ```
     [watchman_test] Received SIGUSR2
     [watchman_test] g_varB changed, new value: 210
     ```

   - The `watchman_test` program will print a message every time it receives `SIGUSR2`, indicating that the monitored variable has changed.

## Notes

- **Threaded Applications**: `watchman` automatically attaches to all threads of the target process.
- **New Threads**: If the process creates new threads after `watchman` has attached, `watchman` will attempt to attach to them due to the `PTRACE_O_TRACECLONE` option.
- **Stopping `watchman`**: To stop `watchman`, you can terminate it with `Ctrl+C`. It will not detach cleanly from the target process, so ensure the target process is managed properly.
- **Error Messages**: `watchman` outputs informative error messages if operations fail. Pay attention to alignment and length errors.
- **Signal Handling in Target Process**: The traced process must have a signal handler for `SIGUSR2` to handle watchpoint hits gracefully.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

**Disclaimer**: Use this tool responsibly and ensure you have permission to monitor the target process. Improper use may violate system policies or laws.
