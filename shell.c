/*
 * shell.c - A simplified Unix-like shell (Mini Command Interpreter)
 *
 * Features:
 *   - Basic commands: ls, cd, pwd, mkdir (plus any other program on PATH)
 *   - I/O redirection:  <   >   >>
 *   - Pipes:            cmd1 | cmd2 | cmd3 ...
 *   - Background jobs:  cmd &
 *   - Built-ins:        cd, pwd, exit, help
 *
 * System calls used: fork(), execvp(), waitpid(), pipe(), dup2(), open(),
 *                    chdir(), getcwd()
 *
 * Build:  gcc -Wall -Wextra -g -o shell shell.c
 * Run:    ./shell          (type 'exit' to go back to bash)
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE   1024
#define MAX_TOKENS 256
#define MAX_ARGS   64
#define MAX_CMDS   16

typedef struct {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile;   /* for <  */
    char *outfile;  /* for >  and >> */
    int   append;   /* 1 if >> */
} Command;

static int  last_status = 0;
static char oldpwd[4096] = "";

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

static void free_tokens(char *tok[], int n)
{
    for (int i = 0; i < n; i++)
        free(tok[i]);
}

/*
 * Splits the line into tokens. Operators (| < > >> &) are recognised even
 * when not surrounded by spaces. Single and double quotes group words.
 * op[i] == 1 means tok[i] is an operator. Returns token count or -1.
 */
static int tokenize(const char *s, char *tok[], int op[])
{
    int n = 0;

    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;

        if (n >= MAX_TOKENS - 1) {
            fprintf(stderr, "shell: too many tokens\n");
            free_tokens(tok, n);
            return -1;
        }

        if (*s == '|' || *s == '<' || *s == '&') {
            tok[n] = strndup(s, 1);
            op[n++] = 1;
            s++;
        } else if (*s == '>') {
            if (s[1] == '>') { tok[n] = strdup(">>"); s += 2; }
            else             { tok[n] = strdup(">");  s += 1; }
            op[n++] = 1;
        } else {
            char buf[MAX_LINE];
            int  len = 0;
            char quote = 0;

            while (*s && (quote || (!isspace((unsigned char)*s) &&
                                    !strchr("|<>&", *s)))) {
                if (quote) {
                    if (*s == quote) quote = 0;
                    else buf[len++] = *s;
                } else if (*s == '"' || *s == '\'') {
                    quote = *s;
                } else {
                    buf[len++] = *s;
                }
                s++;
            }
            if (quote) {
                fprintf(stderr, "shell: unterminated quote\n");
                free_tokens(tok, n);
                return -1;
            }
            buf[len] = '\0';
            tok[n] = strdup(buf);
            op[n++] = 0;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Parser: tokens -> array of Command                                  */
/* Returns 0 on success, -1 on syntax error. *ncmds == 0 for empty.    */
/* ------------------------------------------------------------------ */

static int parse(char *tok[], int op[], int n, Command cmds[],
                 int *ncmds, int *background)
{
    int cur = 0;
    *background = 0;
    *ncmds = 0;
    memset(cmds, 0, sizeof(Command) * MAX_CMDS);

    if (n == 0) return 0;

    for (int i = 0; i < n; i++) {
        if (!op[i]) {
            if (cmds[cur].argc >= MAX_ARGS - 1) {
                fprintf(stderr, "shell: too many arguments\n");
                return -1;
            }
            cmds[cur].argv[cmds[cur].argc++] = tok[i];
            continue;
        }

        if (strcmp(tok[i], "|") == 0) {
            if (cmds[cur].argc == 0) {
                fprintf(stderr, "shell: syntax error near '|'\n");
                return -1;
            }
            if (++cur >= MAX_CMDS) {
                fprintf(stderr, "shell: too many commands in pipeline\n");
                return -1;
            }
        } else if (strcmp(tok[i], "<") == 0 || strcmp(tok[i], ">") == 0 ||
                   strcmp(tok[i], ">>") == 0) {
            if (i + 1 >= n || op[i + 1]) {
                fprintf(stderr, "shell: expected filename after '%s'\n", tok[i]);
                return -1;
            }
            if (tok[i][0] == '<') {
                cmds[cur].infile = tok[i + 1];
            } else {
                cmds[cur].outfile = tok[i + 1];
                cmds[cur].append  = (tok[i][1] == '>');
            }
            i++;
        } else if (strcmp(tok[i], "&") == 0) {
            if (i != n - 1) {
                fprintf(stderr, "shell: '&' must be at the end of the line\n");
                return -1;
            }
            *background = 1;
        }
    }

    if (cmds[cur].argc == 0) {
        fprintf(stderr, "shell: syntax error: missing command\n");
        return -1;
    }

    *ncmds = cur + 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Built-in commands                                                   */
/* ------------------------------------------------------------------ */

static int is_builtin(const char *name)
{
    return strcmp(name, "cd") == 0   || strcmp(name, "pwd") == 0 ||
           strcmp(name, "exit") == 0 || strcmp(name, "help") == 0;
}

static int builtin_cd(Command *c)
{
    char cwd[4096];
    const char *target;

    if (c->argc < 2) {
        target = getenv("HOME");
        if (!target) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
    } else if (strcmp(c->argv[1], "-") == 0) {
        if (!oldpwd[0]) { fprintf(stderr, "cd: OLDPWD not set\n"); return 1; }
        target = oldpwd;
        printf("%s\n", target);
    } else {
        target = c->argv[1];
    }

    if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';

    if (chdir(target) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    strncpy(oldpwd, cwd, sizeof oldpwd - 1);
    return 0;
}

static int builtin_pwd(void)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        perror("pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int builtin_help(void)
{
    puts("Mini Shell - supported features\n"
         "  Built-ins : cd [dir|-], pwd, help, exit\n"
         "  Programs  : ls, mkdir, cat, echo, grep, wc, ... (anything on PATH)\n"
         "  Redirect  : cmd > file   cmd >> file   cmd < file\n"
         "  Pipes     : cmd1 | cmd2 | cmd3\n"
         "  Background: cmd &\n"
         "  Quit      : exit   (or Ctrl+D) - returns you to bash");
    return 0;
}

/* Runs a builtin. 'exit' is handled by the caller in the parent. */
static int run_builtin(Command *c)
{
    if (strcmp(c->argv[0], "cd") == 0)   return builtin_cd(c);
    if (strcmp(c->argv[0], "pwd") == 0)  return builtin_pwd();
    if (strcmp(c->argv[0], "help") == 0) return builtin_help();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Redirection helper (returns 0 on success, -1 on failure)            */
/* ------------------------------------------------------------------ */

static int apply_redirections(Command *c)
{
    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "shell: %s: %s\n", c->infile, strerror(errno));
            return -1;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "shell: %s: %s\n", c->outfile, strerror(errno));
            return -1;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static void run_pipeline(Command cmds[], int n, int background)
{
    int   pipes[MAX_CMDS][2];
    pid_t pids[MAX_CMDS];

    /* Create all pipes up front */
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            last_status = 1;
            return;
        }
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            pids[i] = -1;
            last_status = 1;
            continue;
        }

        if (pid == 0) {
            /* ---------------- child ---------------- */
            signal(SIGINT, background ? SIG_IGN : SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            if (i > 0)     dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < n - 1) dup2(pipes[i][1], STDOUT_FILENO);

            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            /* Background job with no input source: read from /dev/null */
            if (background && i == 0 && !cmds[i].infile) {
                int fd = open("/dev/null", O_RDONLY);
                if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
            }

            if (apply_redirections(&cmds[i]) < 0) _exit(1);

            if (is_builtin(cmds[i].argv[0])) {
                int rc = run_builtin(&cmds[i]);
                fflush(stdout);
                _exit(rc);
            }

            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "shell: %s: %s\n", cmds[i].argv[0], strerror(errno));
            _exit(127);
        }

        pids[i] = pid;
    }

    /* ---------------- parent ---------------- */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (background) {
        printf("[background] pid %d\n", pids[n - 1]);
        return;
    }

    for (int i = 0; i < n; i++) {
        if (pids[i] < 0) continue;
        int status;
        while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR)
            ;
        if (i == n - 1) {
            if (WIFEXITED(status))        last_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
        }
    }
}

/* A single builtin (no pipe) runs in the parent so that cd really works. */
static void run_single_builtin(Command *c)
{
    int saved_in  = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);

    if (apply_redirections(c) == 0) {
        last_status = run_builtin(c);
        fflush(stdout);
    } else {
        last_status = 1;
    }

    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
}

/* Reap finished background jobs so they don't become zombies. */
static void reap_background(void)
{
    int   status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        printf("[done] pid %d\n", pid);
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int main(void)
{
    char  line[MAX_LINE];
    char  cwd[4096];
    char *tok[MAX_TOKENS];
    int   op[MAX_TOKENS];
    Command cmds[MAX_CMDS];

    /* The shell itself must not die on Ctrl+C; children reset this. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    puts("Mini Shell started (child of bash). Type 'help' for commands, 'exit' to return to bash.");

    for (;;) {
        reap_background();

        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
        printf("\033[1;32mmyshell\033[0m:\033[1;34m%s\033[0m\n$ ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) {      /* Ctrl+D */
            putchar('\n');
            break;
        }
        line[strcspn(line, "\n")] = '\0';

        int n = tokenize(line, tok, op);
        if (n <= 0) continue;

        int ncmds, background;
        if (parse(tok, op, n, cmds, &ncmds, &background) == 0 && ncmds > 0) {

            if (ncmds == 1 && strcmp(cmds[0].argv[0], "exit") == 0) {
                free_tokens(tok, n);
                break;
            }

            if (ncmds == 1 && !background && is_builtin(cmds[0].argv[0]))
                run_single_builtin(&cmds[0]);
            else
                run_pipeline(cmds, ncmds, background);
        }

        free_tokens(tok, n);
    }

    puts("Exiting Mini Shell. Back to bash.");
    return last_status;
}
