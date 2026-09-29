# secinspect
# SEC 2211 Practical 2 — Question 1 Guide

Framework for every task: **Build → Observe → Investigate → Explain → Test/Break → Secure → Verify**

Setup (once):
```bash
sudo apt install -y build-essential strace lsof gdb binutils file
mkdir -p "/home/kabunda/Desktop/Syestem P/secinspect"
cd "/home/kabunda/Desktop/Syestem P/secinspect"
cp ~/Downloads/secinspect.c .
gcc -Wall -Wextra -g -o secinspect secinspect.c
printf 'Hello SEC2211\nline two\nline three\n' > test.txt
```
> Note: this working directory has a space in its name (`Syestem P`). Always wrap it in quotes when you `cd` into it
> or refer to it in a script, or escape the space (`Syestem\ P`), otherwise the shell will misread it as two arguments.

> Sources: the assignment PDF plus Lectures 1, 2, 4 and 5. **Lecture 6 was not available**, so the file-descriptor,
> `dup2()`, permission and setuid explanations below come from standard Linux/Unix behaviour. Check the wording against Lecture 6 before presenting.

---

## Task 1.1 — Build `secinspect`  (done)

| Call | Where it shows | Try |
|---|---|---|
| `stat()` | metadata by pathname, before opening | `./secinspect test.txt` |
| `open()` | prints `file descriptor = 3` | same |
| `fstat()` | same metadata by descriptor | same |
| `read()` | reads N bytes from current offset | `-n 10` |
| `lseek()` | `SEEK_CUR`, `SEEK_END`, `SEEK_SET` | `-o 14 -n 10` |
| `write()` | appends at end of file | `-w "new line"` |
| error handling | every call checks `-1` and prints `strerror(errno)` | `./secinspect nofile.txt` |
| `close()` | checked, and extra descriptors closed too | end of every run |

**Be ready to explain:** `stat()` works on a *name*; `fstat()` works on an *open descriptor*. Between the two calls, the name could point to a different file (this idea returns in Q3 as TOCTOU).

---

## Task 1.2 — File descriptors

```bash
./secinspect -e /etc/hostname -e /etc/passwd -l test.txt
```
**Predict first:** stdin, stdout and stderr occupy 0, 1 and 2, so `test.txt` gets 3, then 4 and 5 for the extras.
The kernel always returns the **lowest unused** number.

Sample of what the OS reports (from `/proc/self/fd`, not from our source):
```
fd 0 -> pipe:[...]     (a terminal shows /dev/pts/N here)
fd 1 -> pipe:[...]
fd 2 -> pipe:[...]
fd 3 -> /home/.../test.txt
fd 4 -> /etc/hostname
fd 5 -> /etc/passwd
```
Live view while the process is running: `./secinspect -p test.txt`, then in another terminal:
```bash
ls -l /proc/<PID>/fd
lsof -p <PID>
```

**Why a descriptor is not the file (must be able to say this):**
A descriptor is a small integer, an index into the process's own descriptor table. That table entry points to a kernel *open file description*, which stores the current offset, access mode and flags. That in turn points to the underlying resource (inode/device/pipe/socket). The integer `3` means nothing outside this process. Another process can also have fd 3 pointing at something different.

```
Process → Descriptor table → fd 3 → Open file description (offset, flags) → inode → data on disk
```
This also matches Lecture 4's point that the OS provides three abstractions: **processes, virtual memory, files**.
Files are the abstraction here.

---

## Task 1.3 — Redirection with `dup2()`

```bash
./secinspect -r output.txt test.txt     # terminal shows only the before/after messages (stderr)
cat output.txt                          # the whole report is here
```
Terminal output:
```
[BEFORE dup2]   fd 1 -> /dev/pts/0        (pipe:[...] in my sandbox)
[AFTER dup2(3, 1)]  fd 1 -> /.../output.txt
```

**Explain:**
- Before: FD 1's table entry pointed to the terminal. After `dup2(3, 1)`: FD 1's entry points to the same open file description as FD 3 (`output.txt`).
- `dup2(old, new)` closes `new` if open, then makes `new` refer to the same open file description as `old`. It only rewires the table entry. It **writes no application data**.
- `printf()` still writes to "stdout" (descriptor 1), so it needs no change. Only where descriptor 1 points has changed.
- We call `fflush(stdout)` before `dup2()` so text buffered for the terminal is not accidentally written into the file, and `close(3)` after because FD 1 now holds the file open.

Prove it with strace: `strace -e trace=openat,dup2,close,write ./secinspect -r out.txt test.txt`. You will see
`openat(...) = 3`, `dup2(3, 1) = 1`, `close(3) = 0`, then `write(1, ...)` going to the file.

---

## Task 1.4 — Observe with strace / lsof / /proc

```bash
strace -e trace=openat,read,write,lseek,fstat,newfstatat,close ./secinspect -o 6 -n 8 test.txt
```
**Note:** the `stat()` we call appears in modern glibc as `newfstatat()`. It is the same idea, so say so if asked.

Verified example lines from my run:
```
newfstatat(AT_FDCWD, "test.txt", {st_mode=S_IFREG|0644, st_size=34, ...}, 0) = 0
openat(AT_FDCWD, "out2.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) = 3
dup2(3, 1)                              = 1
close(3)                                = 0
```
Pick three and write **What C requested → Key arguments → What the OS returned → Meaning**, e.g. for `openat`:
- Requested: `open("test.txt", O_RDONLY)`.
- Arguments: `AT_FDCWD` (path relative to current directory), the flags `O_RDONLY`.
- Returned: `3`.
- Meaning: the kernel checked permissions, created an open file description, and gave the lowest free descriptor, 3.

Also capture `lsof -p <PID>` and `ls -l /proc/<PID>/fd` from the paused process (`-p` flag) as extra evidence.

---

## Task 1.5 — From source to running process (Lectures 1, 2, 4)

Lectures 1–2 give the stages: `hello.c` → Preprocessor → Compiler (`.s`) → Assembler (`.o`) → Linker → executable → Loader → running program.

```bash
gcc -E secinspect.c -o secinspect.i      # Preprocessor: #include expanded (mine: ~4,500 lines from ~200)
gcc -S secinspect.c -o secinspect.s      # Compiler: C -> assembly (AT&T syntax, as Lecture 2 says for gcc)
gcc -c secinspect.c -o secinspect.o      # Assembler: machine-code object file
gcc -o secinspect secinspect.o                  # Linker: adds libc references, makes the executable
file secinspect                          # ELF 64-bit ... dynamically linked, interpreter /lib64/ld-linux-x86-64.so.2
readelf -h secinspect                    # ELF header: entry point
readelf -S secinspect | grep -E '\.text|\.data|\.bss|\.rodata'
objdump -d secinspect | less             # disassembly (Lecture 2 tool)
nm secinspect | grep -E 'main|initialized_global'
```
**Loader:** when you type `./secinspect`, the shell calls `fork()` and the child calls `exec()` (Lecture 4), and the loader maps the ELF file into memory. The "interpreter" line in `file`/`readelf -l` is the dynamic loader that maps `libc`.

**Virtual address space (Lecture 4's five segments).** Run:
```bash
./secinspect -m -p test.txt          # prints addresses; then in another terminal:
cat /proc/<PID>/maps
```
Match each printed address to a region in `maps`:

| Lecture 4 segment | Object in secinspect | Where in `/proc/PID/maps` |
|---|---|---|
| Text (code) | `main()` | `r-xp` line of `.../secinspect` |
| Data (initialized) | `initialized_global` | `rw-p` line of `.../secinspect` |
| BSS (uninitialized) | `uninitialized_global` | same `rw-p` region (BSS follows data) |
| Heap | `malloc()` result | `[heap]` |
| Stack | local variable | `[stack]` |
| (shared library) | `printf` | `libc.so.6` |

**A clean thing to demonstrate with GDB** (ASLR is off inside GDB, so addresses are stable):
```bash
nm secinspect | grep main        # main is at file offset 0x1bc4
gdb -q ./secinspect
(gdb) break describe_fd
(gdb) run -r /tmp/g.txt test.txt
(gdb) info proc mappings         # base 0x555555554000, r-xp at 0x555555555000
(gdb) p &initialized_global      # 0x555555559010 -> inside the rw-p region
(gdb) info symbol $rip           # "describe_fd + 8 in section .text"
```
Point to make: `nm` gives a small offset (`0x1bc4`), and at run time the loader adds the base address. In my `-m` run, `main` printed as `0x55c105ed9bc4`,
which ends in the same `bc4`. The addresses change on every run because of ASLR, but the offsets do not.

Also say: these are **virtual** addresses (Lecture 4). The MMU translates them, and each process has its own private address space.

---

## Task 1.6 — Security experiment: permission denied

**Prediction (write before running):** a normal user cannot open a root-only file for reading. `open()` will return `-1` with `EACCES`, because the file mode `600` gives access only to the owner.

```bash
echo "secret" | sudo tee /tmp/secret.txt; sudo chmod 600 /tmp/secret.txt
ls -l /tmp/secret.txt                        # -rw------- root root
id                                           # you are NOT root
./secinspect /tmp/secret.txt                 # fails
strace -e trace=openat ./secinspect /tmp/secret.txt
```
Verified result (run as an unprivileged user):
```
openat(AT_FDCWD, "/tmp/secret.txt", O_RDONLY) = -1 EACCES (Permission denied)
secinspect: open failed on '/tmp/secret.txt': Permission denied (errno=13)
```
**Explain:** the kernel compared the process's effective UID/GID against the file's owner/group/other permission bits.
The check happens inside the `open` system call, in the kernel. That is where enforcement lives.

**Why editing the C source can't bypass it:** our code only *requests* access. The kernel makes the decision, and user-space code cannot change
kernel memory or its access checks. Changing our program just changes what we ask for. The user's identity (UID) is what the kernel checks. The only legitimate ways to get access
are to change permissions, ownership or group membership, or to run as an authorized user (Q2 covers setuid). This links to Lecture 5's principle: trust boundaries, where the receiving side must enforce checks.

> Note: if you test as `root`, the open **succeeds** (root bypasses the permission check), which is why the experiment must be run as a normal user.

---

## Presentation checklist (unprepared questions the lecturer may ask)
- Predict the fd of a newly opened file → lowest free number (3 if nothing else is open).
- Show an open descriptor → `ls -l /proc/<PID>/fd` or `lsof -p`.
- Redirect stdout → `-r file`, then `cat file`.
- Change file offset → `-o N`.
- Find the process in /proc → `pgrep secinspect`, then `/proc/<PID>/`.
- Trace one syscall → `strace -e trace=<name>`.
- Identify a memory region → `-m`, then `/proc/<PID>/maps`.
- Explain a failure → `strerror`/`errno` plus `strace`.
