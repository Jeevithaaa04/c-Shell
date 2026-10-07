# Custom Shell (Mini Command Interpreter)

A simplified Unix-like shell written in C. It runs as a **child process of bash**: when you start it, bash waits (idle) until the shell exits. Type `exit` to return to bash.

## Build & Run (VS Code terminal / Linux / WSL)

```bash
make
./shell
```

```
myshell:/current/path$
```

Type `exit` (or press `Ctrl+D`) to leave the shell and return to bash.

> To confirm it's a subshell: run `echo $$` in bash (bash's PID), then `./shell`, and in a second terminal run `ps -ef --forest | grep shell`. You'll see `shell` under `bash`.

## Supported Commands

### Built-in commands (run inside the shell process)

| Command | Description |
|---|---|
| `cd <dir>` | Change directory |
| `cd` | Go to `$HOME` |
| `cd -` | Go to the previous directory |
| `pwd` | Print current working directory |
| `help` | Show the list of features |
| `exit` | Quit the shell and return to bash |

`cd` and `exit` **must** be built-ins: a child process can't change its parent's directory or end the parent.

### External commands (run via `fork()` + `execvp()`)

Any program on your `PATH` works. The ones from the problem statement:

| Command | Example |
|---|---|
| `ls` | `ls`, `ls -l`, `ls -la /tmp` |
| `mkdir` | `mkdir projects`, `mkdir -p a/b/c` |

Others you can use: `cat`, `echo`, `grep`, `wc`, `sort`, `head`, `tail`, `touch`, `rm`, `rmdir`, `cp`, `mv`, `sleep`, `date`, `whoami`, `ps`, `clear`, etc.

### I/O Redirection

| Syntax | Meaning |
|---|---|
| `cmd > file` | Write stdout to `file` (overwrite) |
| `cmd >> file` | Append stdout to `file` |
| `cmd < file` | Read stdin from `file` |

```
echo hello world > out.txt
cat < out.txt
ls -l >> out.txt
sort < names.txt > sorted.txt
```

### Pipes

Chain any number of commands (up to 16): the stdout of one feeds the stdin of the next.

```
ls -l | grep ".c"
cat out.txt | sort | uniq | wc -l
```

### Background Execution

Add `&` at the end of the line. The shell returns the prompt immediately and prints the PID.

```
sleep 10 &
```

When the job finishes, the shell prints `[done] pid <n>` before the next prompt.

### Other behaviour

- Quotes group words: `echo "hello   world"` or `echo 'a | b'`
- Operators work with or without spaces: `ls>out.txt`, `ls|wc`
- `Ctrl+C` stops the running foreground command but **does not** kill the shell
- `Ctrl+D` at an empty prompt exits (same as `exit`)
- Syntax errors (e.g. `ls |`, `> file`) print a message instead of crashing

## Demo Session (5–6 commands including a pipe and redirection)

```
myshell:~/os$ pwd
/home/user/os
myshell:~/os$ mkdir demo
myshell:~/os$ cd demo
myshell:~/os/demo$ echo "apple banana cherry" > fruits.txt
myshell:~/os/demo$ cat < fruits.txt | wc -w
3
myshell:~/os/demo$ ls -l | grep fruits
-rw-r--r-- 1 user user 20 ... fruits.txt
myshell:~/os/demo$ sleep 5 &
[background] pid 4242
myshell:~/os/demo$ exit
Exiting Mini Shell. Back to bash.
```

## How fork / exec / wait Work (short explanation)

1. **`fork()`** – The shell clones itself. Now there are two nearly identical processes: the parent (the shell) and a child. `fork()` returns `0` in the child and the child's PID in the parent.
2. **`execvp()`** – In the child, `execvp("ls", argv)` *replaces* the child's memory image with the `ls` program. If it succeeds, it never returns. The shell itself is untouched because only the child was replaced.
3. **`waitpid()`** – The parent blocks until the child finishes, then reads its exit status. For background jobs (`&`) the parent skips waiting and reaps the child later (non-blocking `WNOHANG`) so no zombie processes are left behind.

**Pipes:** `pipe()` creates a kernel buffer with a read end and a write end. Before `exec`, each child uses `dup2()` to wire its stdin/stdout to the correct pipe ends.

**Redirection:** The child `open()`s the file and uses `dup2()` to make it stdin (`<`) or stdout (`>` / `>>`) before calling `execvp`.

## Debugging with GDB

```bash
gcc -g -o shell shell.c
gdb ./shell
(gdb) set follow-fork-mode child     # debug the child after fork()
(gdb) break run_pipeline
(gdb) run
```

## Files

| File | Purpose |
|---|---|
| `shell.c` | Shell source code |
| `README.md` | This document |
