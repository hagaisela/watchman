# watchman

**`watchman`** is a lightweight debugging tool that uses **hardware watchpoints** to monitor memory writes in running processes. It attaches to a live process, sets up to 4 watchpoints using the `ptrace` API (DR0–DR3 on x86), and triggers symbolic stack traces in the target process when a watchpoint is hit. The tool leverages **libbacktrace** for symbolic decoding of instruction pointers.

---

## Features

- **Hardware watchpoints**: Monitor up to 4 memory locations simultaneously using debug registers (DR0–DR3).  
- **Signal-based communication**: Sends `SIGUSR2` to the target process when a watchpoint is triggered.  
- **In-process stack traces**: The target process uses `libbacktrace` to print a stack trace, including file names and line numbers.  
- **Easy integration**: Includes example programs to demonstrate setting up watchpoints and monitoring memory writes.

---

## Components

- **`multi_watch_test`**: A sample program that modifies multiple global variables in a loop. The debugger attaches to this program and monitors memory writes.  
- **`watchman`**: The external debugger that attaches to a running process, sets watchpoints, and signals the target process when a watchpoint is triggered.

---

## How It Works

1. The **debugger** attaches to a running process using `ptrace`.  
2. It sets hardware watchpoints for specific memory addresses.  
3. When a watchpoint is triggered:
   - The debugger detects the event via `ptrace` and injects a `SIGUSR2` signal into the target.  
   - The target’s custom `SIGUSR2` handler generates a **local stack trace** using `libbacktrace`.  
4. The target process resumes execution after handling the signal.

---

## Requirements

- **Linux**: The project uses the `ptrace` API, which is Linux-specific.
- **libbacktrace**: Ensure `libbacktrace` is installed for symbolic stack traces.
  - Install on Ubuntu/Debian: `sudo apt install libbacktrace-dev`.

---

## Build Instructions

Clone the repository and build the project using `make`:

```bash
git clone https://github.com/yourusername/watchman.git
cd watchman
make
