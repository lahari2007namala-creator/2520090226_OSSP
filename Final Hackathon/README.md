# Mini Linux Shell with Background Job Management

## Overview

This project implements a mini Linux shell in C with a Python/Tkinter desktop dashboard. Users can execute Linux commands, run programs in the background, view job information, and terminate selected jobs.

The application uses real Linux processes and demonstrates process creation, signal handling, foreground/background execution, and child-process cleanup.

## Features

- Execute installed Linux commands with arguments.
- Run commands in the background using `&`.
- List background jobs with their job IDs, process IDs, commands, and states.
- Terminate jobs using commands or dashboard controls.
- Bring jobs to the foreground.
- Stop and resume jobs.
- Change the current directory.
- Detect completed child processes and collect their exit status.
- Display command output and job information in a desktop dashboard.
- Clean up active tracked jobs when the shell exits normally.

## Technologies Used

| Technology | Purpose |
|------------|---------|
| C11 | Shell implementation and process management |
| Linux/POSIX functions | Process creation, signals, and terminal control |
| Python 3 | Desktop application |
| Tkinter | Dashboard interface |
| Pseudo-terminal (PTY) | Terminal input and output |
| Unix-domain socket and JSON | Transfer job information to the dashboard |
| GCC and Make | Compilation and build automation |
| Python unittest | Integration testing |

## Requirements

- Ubuntu/Linux or Ubuntu running through WSL on Windows.
- GCC and Make.
- Python 3.10 or later.
- Tkinter.
- A graphical Linux desktop or WSLg for the dashboard.

The command-line version does not require a graphical display.

## Setup Instructions

1. Download and extract the project.
2. Open an Ubuntu terminal in the project folder containing `mini_shell.c`, `gui.py`, and `Makefile`.
3. Install the required dependencies:

```bash
sudo apt update
sudo apt install -y build-essential python3 python3-tk
```

Run all remaining build and execution commands from this project folder.

## Compilation

Compile the C source using:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 mini_shell.c -o mini_shell
```

Alternatively, use:

```bash
make
```

Successful compilation creates the `mini_shell` executable.

## Execution

### Desktop Dashboard

After compilation, start the dashboard:

```bash
python3 gui.py
```

Alternatively, use the launcher to build and open the application:

```bash
bash run.sh
```

Enter commands in the input box and press Enter or click **Run / Send**.

### Command-Line Version

Start the shell directly:

```bash
./mini_shell
```

Enter commands at the `mini-shell>` prompt.

To end the session:

```text
exit
```

## Example Usage

Enter each command separately:

```text
pwd
ls -l
echo "Hello from the mini shell"
sleep 300 &
jobs
```

The final `&` starts a command in the background, allowing the shell to accept another command immediately.

To terminate a job:

```text
kill %1
```

Replace `%1` with the actual job ID displayed by `jobs`.

Every external command receives a job ID, including foreground commands. A job ID is assigned by the shell; a PID is assigned by Linux.

## Built-In Commands

| Command | Description |
|---------|-------------|
| `help` | Display supported commands |
| `pwd` | Display the current directory |
| `cd /path` | Change the working directory |
| `cd ..` | Move to the parent directory |
| `cd` | Move to the Linux user's home directory |
| `jobs` | List tracked background jobs |
| `fg %ID` | Bring an active job to the foreground |
| `bg %ID` | Resume a stopped job in the background |
| `kill %ID` | Send SIGTERM to a tracked job |
| `kill -STOP %ID` | Stop a tracked job |
| `kill -CONT %ID` | Continue a tracked job |
| `kill -INT %ID` | Send SIGINT to a tracked job |
| `kill -KILL %ID` | Forcefully terminate a tracked job |
| `exit` | Clean up tracked jobs and end the session |

Replace `%ID` with the required job ID.

## Dashboard Controls

| Control | Function |
|---------|----------|
| Run / Send | Submit a command or input to the foreground program |
| Foreground | Give the selected active job terminal control |
| Stop ^Z | Pause the foreground job |
| Resume | Continue a selected stopped job in the background |
| Terminate | Request termination of the selected active job |
| Interrupt ^C | Interrupt the foreground job |
| End input ^D | Send end-of-input |
| Clear view | Clear displayed output while jobs continue |

At an empty shell prompt, **End input** ends the session. Close the dashboard and run `bash run.sh` to start another session.

## Implementation

1. The dashboard sends commands to the C shell through a PTY.
2. The shell parses commands and handles built-ins.
3. External programs launch using `fork()` and `execvp()`.
4. Process groups and terminal ownership provide foreground/background control.
5. Signals control stopping, continuation, and termination.
6. `waitpid()` collects child-process state changes and exit status.
7. Updated job information is sent to the dashboard through a private local socket.

Linux performs CPU scheduling. The application manages command execution and job control.

## Testing

Run the automated integration tests:

```bash
make test
```

The suite contains 10 tests covering command execution, quoting, directory changes, background responsiveness, process completion, stop/resume operations, termination, invalid input, and exit cleanup.

A successful run reports:

```text
Ran 10 tests
OK
```

For a dashboard startup and shutdown check in a graphical session:

```bash
python3 gui.py --smoke-test
```

## Project Files

| File | Description |
|------|-------------|
| `mini_shell.c` | C shell implementation |
| `gui.py` | Desktop dashboard |
| `ui_backend.py` | PTY and status-channel communication |
| `Makefile` | Build, run, and test commands |
| `run.sh` | Application launcher |
| `tests/test_shell.py` | Integration tests |
| `README.md` | Setup and execution instructions |
| `REPORT.md` | Technical project report |
| `TESTING.md` | Testing and validation details |
| `DEMO.md` | Demonstration instructions |

## Limitations

- One directly launched external program per job.
- A maximum of 32 active jobs.
- Pipes, redirection, command chaining, wildcard expansion, and variable expansion are not implemented.
- The dashboard supports line-oriented output rather than full terminal emulation.
- Cleanup covers tracked jobs; daemonizing programs and descendants that escape their process group are outside its guarantee.

## Responsible Use

Run controlled commands and processes during demonstrations. External programs execute with the current user's normal permissions. The job-control built-ins operate on jobs tracked by this shell.
