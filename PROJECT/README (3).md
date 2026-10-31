# Team 20 - Linux Signal-Based Process Communication and Control System

Course: Operating Systems and Systems Programming (25CS2104E), Section 13, Team 20
Platform: Ubuntu Linux or Ubuntu on WSL2 (Windows)
Language: C11 with POSIX APIs; tests use Python 3 standard library only

---

## 1. Prerequisites (one time)

Open an Ubuntu terminal (WSL2: Start menu -> Ubuntu) and run:

```bash
sudo apt update
sudo apt install -y build-essential python3 unzip zip
gcc --version
python3 --version
```

---

## 2. Project structure

```
team20-project/
├── Makefile               build, run, test, clean
├── README.md              this file
├── src/
│   └── team20.c           controller and worker code (single file)
└── tests/
    └── test_team20.py     13 automated tests (unittest)
```

Runtime files created by the program:

| File | Purpose | Removed on quit? |
|---|---|---|
| `/tmp/team20_ctl` | named FIFO for commands from a second terminal | yes |
| `team20_events.log` | timestamped event log (in the folder where you ran `./team20`) | no (kept as evidence) |

---

## 3. Get the project files

If you have the zip file `team20-project.zip`:

```bash
cd ~
cp /mnt/c/Users/<your-windows-user>/Downloads/team20-project.zip ~/
unzip team20-project.zip
cd ~/team20-project
ls
```

Replace `<your-windows-user>` with your Windows user name (for example `surya`).
The `ls` output should list `Makefile`, `README.md`, `src`, `tests`.

---

## 4. Build

```bash
cd ~/team20-project
make
```

Expected: one `gcc ... -o team20 src/team20.c -pthread` line and no warnings.

---

## 5. Run the automated tests

```bash
cd ~/team20-project
make test 2>&1 | tee test_output.txt
```

Expected last line: `OK`. The output is saved to `test_output.txt`.
`ResourceWarning` lines are harmless messages from the test script.

---

## 6. Run the program (Terminal 1)

Open Ubuntu Terminal 1, then:

```bash
cd ~/team20-project
make run
```

Expected output (PIDs will differ):

```
controller ready: pid=..., worker group=..., FIFO=/tmp/team20_ctl, log=team20_events.log
[child] READY 1 ...
[child] READY 2 ...
[child] READY 3 ...
```

Leave Terminal 1 running. Do not type shell commands such as `cd` or `make` here;
the program reads only its own commands (see section 7).

---

## 7. Send commands from Terminal 2 (FIFO)

Open a second Ubuntu terminal (Terminal 2). Each command is one `echo` into the FIFO.
Results appear in Terminal 1.

```bash
echo "help" > /tmp/team20_ctl
echo "list" > /tmp/team20_ctl
echo "status all" > /tmp/team20_ctl
echo "status 1" > /tmp/team20_ctl
echo "work 2" > /tmp/team20_ctl
echo "work all" > /tmp/team20_ctl
echo "stop 2" > /tmp/team20_ctl
echo "list" > /tmp/team20_ctl
echo "cont 2" > /tmp/team20_ctl
echo "stop all" > /tmp/team20_ctl
echo "cont all" > /tmp/team20_ctl
echo "board" > /tmp/team20_ctl
echo "mem" > /tmp/team20_ctl
echo "files" > /tmp/team20_ctl
echo "bogus" > /tmp/team20_ctl
echo "quit" > /tmp/team20_ctl
```

What to look for:
- `list` after `stop 2` shows `child 2 ... state=T (stopped)`.
- `list` after `cont 2` shows `state=S (sleeping)` again.
- `board` shows `BOARD work=[a b c] total=...`.
- `bogus` prints `ERR unknown command 'bogus'`.

---

## 8. Commands reference

| Command | Signal / action | Notes |
|---|---|---|
| `help` | none | prints the command list |
| `list` | none | PID, PPID, PGID and kernel state from `/proc/<pid>/stat` |
| `status <1\|2\|3\|all>` | SIGUSR1 | child replies with a STATUS line |
| `work <1\|2\|3\|all>` | SIGUSR2 | child handles one work event; counted on the board |
| `stop <1\|2\|3\|all>` | SIGSTOP | `all` uses `kill(-pgid, ...)` |
| `cont <1\|2\|3\|all>` | SIGCONT | resumes stopped children |
| `board` | none | snapshot of shared memory (semaphore-protected, 300 ms wait) |
| `mem` | none | VmSize, VmRSS, Threads from `/proc/<pid>/status` |
| `files` | none | open fds from `/proc/self/fd`; `stat()` of log and FIFO |
| `quit` | controlled shutdown | same as Ctrl+C, SIGTERM or closing stdin |

Shutdown triggers: `quit`, Ctrl+C in Terminal 1, `kill -TERM <controller pid>`, Ctrl+D in Terminal 1.

---

## 9. Stop the program and confirm cleanup

Stop it with `quit` in Terminal 2 (or Ctrl+C in Terminal 1). Expected last lines in Terminal 1:

```
shutdown complete: all workers reaped
team20 exited cleanly
```

Confirm cleanup in Terminal 2:

```bash
ls -l /tmp/team20_ctl 2>&1
pgrep -a team20 || echo "no team20 processes left"
```

Expected: `No such file or directory` for the FIFO and `no team20 processes left`.

If an old copy is still running (for example after an interrupted test):

```bash
pkill -x team20
```

---

## 10. Read the log

```bash
cd ~/team20-project
cat team20_events.log                         # full log
tail -n 30 team20_events.log                  # last 30 lines
grep -E "stop|cont|state=" team20_events.log  # CO2 evidence
```

Each line starts with a timestamp, for example `[2026-10-31 08:04:35.423] stop sent to child 2 (pid 1366)`.

---

## 11. Save evidence for submission

```bash
mkdir -p ~/team20-submission/evidence
cd ~/team20-project
cp test_output.txt team20_events.log ~/team20-submission/evidence/
grep -E "stop|cont|state=" team20_events.log > ~/team20-submission/evidence/co2_evidence.txt
ls -l ~/team20-submission/evidence
```

Copy screenshots from Windows into the same folder:
`\\wsl$\Ubuntu\home\<your-linux-user>\team20-submission\evidence`

---

## 12. Clean up build files

Stop the program first (section 9), then:

```bash
cd ~/team20-project
make clean
```

This removes `team20`, `team20_events.log` and `tests/__pycache__`.
It does not remove `test_output.txt` or the evidence folder.

---

## 13. Troubleshooting

| Message or symptom | Cause and fix |
|---|---|
| `another team20 instance is running` | A controller already owns the FIFO. Quit it (`quit` or Ctrl+C), then retry. |
| `removed stale FIFO left by an earlier run` | Normal after a crash. Nothing to do. |
| `... exists and is not a FIFO. Remove it manually.` | Another file is at `/tmp/team20_ctl`. Check `ls -l /tmp/team20_ctl`, then delete it yourself. |
| `ERR unknown command 'cd'` or `'make'` in Terminal 1 | You typed shell commands into the running program. Quit it, then run shell commands. |
| `BOARD busy` | A stopped child holds the lock. Run `cont all`, then `board` again. |
| Program exits immediately | stdin was closed (EOF). Run `make run` from a normal terminal. |
| `make: gcc: not found` | `sudo apt install -y build-essential` |
| `unzip: cannot find or open` | The zip is not in `~`. Copy it from the Windows Downloads folder (section 3). |
| `make: *** No targets specified` | You are in the wrong folder. Run `cd ~/team20-project` first. |

---

## 14. Known limitations

- Standard signals can merge. 150 `work` signals give fewer handled events; the board
  counts only events that the children actually handled.
- Workers are forked copies of the same program (no `exec()`).
- The FIFO and log use fixed paths, so only one controller can run at a time.
- Stop/continue controls whether a process may run. It does not implement a CPU scheduler.

---

## 15. Verification status

- Build with `-std=c11 -Wall -Wextra -Wpedantic -pthread`: no warnings.
- `make test`: 13 tests pass (startup and process groups, stop/continue checked in `/proc`,
  group stop/continue, work counts, 150-signal burst, status/mem/files, invalid and overlong
  input, quit, SIGTERM and SIGINT with a stopped worker, stdin EOF, second-instance refusal,
  repeated startup and shutdown).
- Manual runs on Ubuntu under WSL2 confirmed the keyboard and FIFO commands, stop/continue
  states, clean shutdown and cleanup.

---

## 16. CO mapping (what this project demonstrates)

| CO | Concept | Main code in `src/team20.c` |
|---|---|---|
| CO1 | system calls, error checks, user vs kernel space | `fork`, `kill`, `waitpid`, `pipe`, `read`/`write`, `sigaction`, `/proc` reads |
| CO2 | process creation, groups, stop/continue, reaping | `child_main`, `cmd_signal`, `reap_children`, `shutdown_children`, `cmd_list` |
| CO3 | signals, pipe, FIFO, signal masks, group signals | `child_sig`, `parent_sig`, `sigsuspend`, `ppoll`, `handle_command` |
| CO4 | threads, mutex, condition variables, semaphore | `logger_main`, `lq_put`, `log_stop`, `board_lock`, `board_trylock_ms` |
| CO5 | mmap shared memory, private vs shared data, memory from `/proc` | `main` (mmap), `Board`, `private_work`, `cmd_mem` |
| CO6 | file descriptors, open flags, log file, `stat()`, FIFO, `unlink()` | `cmd_files`, `mkfifo` in `main`, `unlink` in `teardown`, `fopen "a"` |

Syllabus check: the official CO wording was not attached. This mapping follows the review
decks. Confirm it against the official syllabus before submission.
