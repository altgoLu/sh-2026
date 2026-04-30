#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>

//
// You should use the following functions to print information
// Do not modify these functions
//

// Track virtual environment state
static int env_active = 0;
static char* env_path = NULL;
static char* original_path = NULL;

void print_prompt() {
    if (env_active) {
        printf("(env) sh > ");
    } else {
        printf("sh > ");
    }
    fflush(stdout);
}

void print_invalid_syntax() {
    printf("Invalid Syntax\n");
    fflush(stdout);
}

void print_command_not_found() {
    printf("Command Not Found\n");
    fflush(stdout);
}

void print_execution_error() {
    printf("Execution Error\n");
    fflush(stdout);
}

void print_blocked_syscall(char* syscall_name, int count, ...) {
    va_list args;
    va_start(args, count);
    printf("Blocked Syscall: %s ", syscall_name);
    for (int i = 0; i < count; i++) {
        char* arg = va_arg(args, char*);
        printf("%s ", arg);
    }
    printf("\n");
    fflush(stdout);
}

#ifdef MAX_INPUT
#undef MAX_INPUT
#endif

#define MAX_ARGS 32
#define MAX_CMDS 16
#define MAX_INPUT 1024
#define MAX_TOKENS 128
#define EXIT_CMD_NOT_FOUND 127
#define EXIT_EXEC_ERROR 126

char *input = NULL;
size_t input_cap = 0;

struct command {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
    int append;
};

struct job {
    struct command cmds[MAX_CMDS];
    int cmd_count;
};

struct token {
    enum token_type {
        WORD,
        PIPE,
        REDIRECT_IN,
        REDIRECT_OUT,
        REDIRECT_APPEND
    } type;
    char *value;
} tokens[MAX_TOKENS];
int ntok;

void free_tokens() {
    for (int i = 0; i < ntok; i++) {
        if (tokens[i].type == WORD) {
            free(tokens[i].value);
            tokens[i].value = NULL;
        }
    }
    ntok = 0;
}

void tokenizer(char *line) {
    int len = strlen(line); ntok = 0;
    for(int i = 0; i < len;) {
        if(isspace(line[i])) {
            i++;
            continue;
        }
        if(line[i] == '|') {
            tokens[ntok].type = PIPE;
            tokens[ntok].value = NULL;
            i++;
        } else if(line[i] == '>') {
            if(i + 1 < len && line[i + 1] == '>') {
                tokens[ntok].type = REDIRECT_APPEND;
                tokens[ntok].value = NULL;
                i += 2;
            } else {
                tokens[ntok].type = REDIRECT_OUT;
                tokens[ntok].value = NULL;
                i++;
            }
        } else if(line[i] == '<') {
            tokens[ntok].type = REDIRECT_IN;
            tokens[ntok].value = NULL;
            i++;
        } else {
            int j = i;
            while(j + 1 < len && !isspace(line[j + 1]) && line[j + 1] != '|' &&
                line[j + 1] != '>' && line[j + 1] != '<') {
                j++;
            }
            tokens[ntok].type = WORD;
            tokens[ntok].value = strndup(line + i, j - i + 1);
            i = j + 1;
        }
        ntok++;
    }
    return;
}

static void init_command(struct command *cmd) {
    cmd->argc = 0;
    cmd->infile = NULL;
    cmd->outfile = NULL;
    cmd->append = 0;
    for (int i = 0; i < MAX_ARGS; i++) {
        cmd->argv[i] = NULL;
    }
}

static void init_job(struct job *j) {
    j->cmd_count = 0;
    for (int i = 0; i < MAX_CMDS; i++) {
        init_command(&j->cmds[i]);
    }
}

static int command_is_empty(const struct command *cmd) {
    return cmd->argc == 0 && cmd->infile == NULL && cmd->outfile == NULL;
}

static int finish_command(struct job *j, struct command *cmd) {
    if (cmd->argc == 0) {
        return 0;
    }
    if (j->cmd_count >= MAX_CMDS) {
        return 0;
    }
    cmd->argv[cmd->argc] = NULL;
    j->cmds[j->cmd_count++] = *cmd;
    init_command(cmd);
    return 1;
}

int parser(struct job *j) {
    init_job(j);
    if (ntok == 0) {
        return 1;
    }

    struct command current;
    init_command(&current);

    for (int i = 0; i < ntok; i++) {
        switch (tokens[i].type) {
        case WORD:
            if (current.argc >= MAX_ARGS - 1) {
                return 0;
            }
            current.argv[current.argc++] = tokens[i].value;
            break;
        case REDIRECT_IN:
            if (current.infile != NULL || i + 1 >= ntok || tokens[i + 1].type != WORD) {
                return 0;
            }
            current.infile = tokens[++i].value;
            break;
        case REDIRECT_OUT:
        case REDIRECT_APPEND:
            if (current.outfile != NULL || i + 1 >= ntok || tokens[i + 1].type != WORD) {
                return 0;
            }
            current.outfile = tokens[++i].value;
            current.append = (tokens[i - 1].type == REDIRECT_APPEND);
            break;
        case PIPE:
            if (!finish_command(j, &current)) {
                return 0;
            }
            break;
        }
    }

    if (!command_is_empty(&current)) {
        if (!finish_command(j, &current)) {
            return 0;
        }
    }

    return j->cmd_count > 0;
}

static int validate_execution_plan(struct job *j) {
    for (int i = 0; i < j->cmd_count; i++) {
        if (j->cmds[i].infile != NULL || j->cmds[i].append) {
            return 0;
        }
        if (i < j->cmd_count - 1 && j->cmds[i].outfile != NULL) {
            return 0;
        }
    }
    return 1;
}

static int builtin_command_id(struct command *cmd) {
    if (cmd->argc == 0) {
        return 0;
    }
    if (strcmp(cmd->argv[0], "cd") == 0) {
        return 1;
    }
    if (strcmp(cmd->argv[0], "exit") == 0) {
        return 2;
    }
    if (strcmp(cmd->argv[0], "env-use") == 0) {
        return 3;
    }
    if (strcmp(cmd->argv[0], "env-exit") == 0) {
        return 4;
    }
    return 0;
}

static void close_all_pipes(int pipes[][2], int pipe_count) {
    for (int i = 0; i < pipe_count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
}

static int setup_child_io(struct job *j, int index, int pipes[][2]) {
    struct command *cmd = &j->cmds[index];

    if (index > 0 && dup2(pipes[index - 1][0], STDIN_FILENO) == -1) {
        return 0;
    }

    if (cmd->outfile != NULL) {
        int fd = open(cmd->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            return 0;
        }
        if (dup2(fd, STDOUT_FILENO) == -1) {
            close(fd);
            return 0;
        }
        close(fd);
    } else if (index < j->cmd_count - 1 && dup2(pipes[index][1], STDOUT_FILENO) == -1) {
        return 0;
    }

    return 1;
}

static void run_external_command(struct command *cmd) {
    execvp(cmd->argv[0], cmd->argv);

    if (errno == ENOENT) {
        _exit(EXIT_CMD_NOT_FOUND);
    }
    _exit(EXIT_EXEC_ERROR);
}

static void handle_child_status(int status) {
    if (!WIFEXITED(status)) {
        print_execution_error();
        return;
    }

    int code = WEXITSTATUS(status);
    if (code == 0) {
        return;
    }
    if (code == EXIT_CMD_NOT_FOUND) {
        print_command_not_found();
        return;
    }
    print_execution_error();
}

static int init_shell_env(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return 0;
    }

    if (clearenv() != 0 ||
        setenv("PATH", "/bin", 1) != 0 ||
        setenv("HOME", cwd, 1) != 0 ||
        setenv("PWD", cwd, 1) != 0 ||
        setenv("OLDPWD", cwd, 1) != 0 ||
        setenv("LANG", "en_US.UTF-8", 1) != 0 ||
        setenv("SH_VERSION", "1.0", 1) != 0) {
        return 0;
    }

    return 1;
}

static int run_cd(struct command *cmd) {
    if (cmd->argc != 2) {
        print_invalid_syntax();
        return 0;
    }

    char oldpwd[PATH_MAX];
    char expanded_target[PATH_MAX];
    if (getcwd(oldpwd, sizeof(oldpwd)) == NULL) {
        print_execution_error();
        return 0;
    }

    char *target = cmd->argv[1];
    if (strcmp(target, "~") == 0) {
        target = getenv("HOME");
    } else if (strncmp(target, "~/", 2) == 0) {
        char *home = getenv("HOME");
        if (home == NULL) {
            print_execution_error();
            return 0;
        }
        if (snprintf(expanded_target, sizeof(expanded_target), "%s/%s", home, target + 2) >= (int)sizeof(expanded_target)) {
            print_execution_error();
            return 0;
        }
        target = expanded_target;
    } else if (strcmp(target, "-") == 0) {
        target = getenv("OLDPWD");
    }

    if (target == NULL || chdir(target) != 0) {
        print_execution_error();
        return 0;
    }

    char newpwd[PATH_MAX];
    if (getcwd(newpwd, sizeof(newpwd)) == NULL) {
        print_execution_error();
        return 0;
    }

    if (setenv("OLDPWD", oldpwd, 1) != 0 || setenv("PWD", newpwd, 1) != 0) {
        print_execution_error();
    }

    return 0;
}

static int run_env_use(struct command *cmd) {
    if (cmd->argc != 2) {
        print_invalid_syntax();
        return 0;
    }

    if (!env_active) {
        char *path = getenv("PATH");
        if (path == NULL) {
            print_execution_error();
            return 0;
        }

        original_path = strdup(path);
        if (original_path == NULL) {
            print_execution_error();
            return 0;
        }
    }

    size_t new_path_len = strlen(cmd->argv[1]) + strlen("/bin:") + strlen(original_path) + 1;
    char *new_path = malloc(new_path_len);
    if (new_path == NULL) {
        print_execution_error();
        return 0;
    }

    snprintf(new_path, new_path_len, "%s/bin:%s", cmd->argv[1], original_path);
    if (setenv("PATH", new_path, 1) != 0) {
        free(new_path);
        print_execution_error();
        return 0;
    }

    free(new_path);
    env_active = 1;
    return 0;
}

static int run_env_exit(void) {
    if (!env_active) {
        return 0;
    }

    if (original_path == NULL || setenv("PATH", original_path, 1) != 0) {
        print_execution_error();
        return 0;
    }

    free(original_path);
    original_path = NULL;
    env_active = 0;
    return 0;
}

int execute(struct job *j) {
    if (j->cmd_count == 0) {
        return 0;
    }

    if (!validate_execution_plan(j)) {
        print_invalid_syntax();
        return 0;
    }

    int builtin_id = 0;
    if (j->cmd_count == 1) {
        builtin_id = builtin_command_id(&j->cmds[0]);
    }
    if (builtin_id) {
        switch (builtin_id) {
        case 1:
            return run_cd(&j->cmds[0]);
        case 2:
            return 1;
        case 3:
            return run_env_use(&j->cmds[0]);
        case 4:
            return run_env_exit();
        }
    }

    int pipe_count = j->cmd_count - 1;
    int pipes[MAX_CMDS - 1][2];
    pid_t pids[MAX_CMDS];

    for (int i = 0; i < pipe_count; i++) {
        if (pipe(pipes[i]) == -1) {
            close_all_pipes(pipes, i);
            print_execution_error();
            return 0;
        }
    }

    for (int i = 0; i < j->cmd_count; i++) {
        pids[i] = fork();
        if (pids[i] == -1) {
            close_all_pipes(pipes, pipe_count);
            print_execution_error();
            return 0;
        }

        if (pids[i] == 0) {
            if (!setup_child_io(j, i, pipes)) {
                _exit(EXIT_EXEC_ERROR);
            }
            close_all_pipes(pipes, pipe_count);
            run_external_command(&j->cmds[i]);
        }
    }

    close_all_pipes(pipes, pipe_count);

    for (int i = 0; i < j->cmd_count; i++) {
        int status = 0;
        if (waitpid(pids[i], &status, 0) == -1) {
            print_execution_error();
            continue;
        }
        handle_child_status(status);
    }

    return 0;
}

int main() {
    if (!init_shell_env()) {
        print_execution_error();
        return 1;
    }

    while(1) {
        print_prompt();
        ssize_t input_len = getline(&input, &input_cap, stdin);
        if (input_len == -1) {
            break;
        }
        if (input_len >= MAX_INPUT) {
            print_invalid_syntax();
            continue;
        }
        tokenizer(input);
        struct job j;
        if (!parser(&j)) {
            print_invalid_syntax();
        } else {
            if (execute(&j)) {
                free_tokens();
                break;
            }
        }


        free_tokens();
    }
    free(input);
    return 0;
}
