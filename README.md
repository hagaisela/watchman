# hw-watch

**`hw-watch`** is a lightweight debugging tool that uses **hardware watchpoints** to monitor memory writes in running processes. It attaches to a live process, sets up to 4 watchpoints using the `ptrace` API (DR0–DR3 on x86), and triggers symbolic stack traces in the target process when a watchpoint is hit. The tool leverages **libbacktrace** for symbolic decoding of instruction pointers.

---

## Features

- **Hardware watchpoints**: Set up to 4 simultaneous watchpoints (DR0–DR3) to monitor specific memory addresses.
- **Signal-based communication**: Sends custom signals (e.g., `SIGUSR1`) to the target process to trigger local stack traces.
- **In-process stack traces**: The target process itself uses `libbacktrace` to decode its stack, providing precise file names and line numbers.
- **Simple integration**: Includes a test program (`multi_watch_test`) to demonstrate hardware watchpoints in action.

---

## Components

- **`multi_watch_test`**: A test program that modifies multiple global variables. The debugger attaches to this process and monitors memory writes.  
- **`watch_debugger`**: The external debugger that attaches to a running process, sets watchpoints, and signals the target process when a watchpoint is triggered.

---

## How It Works

1. **Attach** the debugger to a running process using `ptrace`.
2. **Set watchpoints** for specific memory addresses (e.g., `g_varA`, `g_varB`).
3. When a watchpoint is triggered:
   - The debugger detects the event via `ptrace` and injects a `SIGUSR1` signal into the target.
   - The target handles `SIGUSR1` and generates a **local stack trace** using `libbacktrace`.
4. Execution continues seamlessly after the watchpoint.

---

## Requirements

- **Linux**: The project uses the `ptrace` API, which is Linux-specific.
- **libbacktrace**: Ensure `libbacktrace` is installed for symbolic stack traces.
  - Install on Ubuntu/Debian: `sudo apt install libbacktrace-dev`.

---

## Build Instructions

Clone the repository and build the project using `make`:

```bash
git clone https://github.com/yourusername/hw-watch.git
cd hw-watch
make
