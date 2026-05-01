#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>
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
#define MAX_RULES 32
#define MAX_RULE_ARGS 6
#define MAX_RULE_STR 256
#define MAX_DISPLAY_STR 1024
#define MAX_TRACEES 128
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

struct tracee_state {
    pid_t pid;
    int alive;
    int in_syscall;
    int options_set;
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

enum deny_arg_type {
    DENY_ARG_NONE,
    DENY_ARG_INT,
    DENY_ARG_STRING
};

struct deny_arg {
    int specified;
    enum deny_arg_type type;
    unsigned long int_value;
    char str_value[MAX_RULE_STR];
};

struct deny_rule {
    int syscall_no;
    char syscall_name[32];
    struct deny_arg args[MAX_RULE_ARGS];
};

struct deny_list {
    struct deny_rule rules[MAX_RULES];
    int rule_num;
} deny_list;

void free_tokens() {
    for (int i = 0; i < ntok; i++) {
        if (tokens[i].type == WORD) {
            free(tokens[i].value);
            tokens[i].value = NULL;
        }
    }
    ntok = 0;
}

static int push_token(enum token_type type, const char *value) {
    if (ntok >= MAX_TOKENS) {
        return 0;
    }

    tokens[ntok].type = type;
    tokens[ntok].value = NULL;
    if (type == WORD) {
        tokens[ntok].value = strdup(value);
        if (tokens[ntok].value == NULL) {
            return 0;
        }
    }
    ntok++;
    return 1;
}

int tokenizer(char *line) {
    int len = strlen(line);
    ntok = 0;
    for (int i = 0; i < len;) {
        if (isspace((unsigned char)line[i])) {
            i++;
            continue;
        }

        if (line[i] == '|') {
            if (!push_token(PIPE, NULL)) {
                free_tokens();
                return 0;
            }
            i++;
        } else if (line[i] == '>') {
            if (i + 1 < len && line[i + 1] == '>') {
                if (!push_token(REDIRECT_APPEND, NULL)) {
                    free_tokens();
                    return 0;
                }
                i += 2;
            } else {
                if (!push_token(REDIRECT_OUT, NULL)) {
                    free_tokens();
                    return 0;
                }
                i++;
            }
        } else if (line[i] == '<') {
            if (!push_token(REDIRECT_IN, NULL)) {
                free_tokens();
                return 0;
            }
            i++;
        } else {
            char word[MAX_INPUT];
            int word_len = 0;

            while (i < len &&
                   !isspace((unsigned char)line[i]) &&
                   line[i] != '|' &&
                   line[i] != '>' &&
                   line[i] != '<') {
                if (line[i] == '\'' || line[i] == '"') {
                    char quote = line[i++];
                    while (i < len && line[i] != quote) {
                        if (word_len >= MAX_INPUT - 1) {
                            free_tokens();
                            return 0;
                        }
                        word[word_len++] = line[i++];
                    }
                    if (i >= len) {
                        free_tokens();
                        return 0;
                    }
                    i++;
                } else {
                    if (word_len >= MAX_INPUT - 1) {
                        free_tokens();
                        return 0;
                    }
                    word[word_len++] = line[i++];
                }
            }

            word[word_len] = '\0';
            if (!push_token(WORD, word)) {
                free_tokens();
                return 0;
            }
        }
    }

    return 1;
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

static int sandbox_syscall_number(const char *name) {
    if (strcmp(name, "read") == 0) return 0;
    if (strcmp(name, "write") == 0) return 1;
    if (strcmp(name, "open") == 0) return 2;
    if (strcmp(name, "pipe") == 0) return 22;
    if (strcmp(name, "dup") == 0) return 32;
    if (strcmp(name, "clone") == 0) return 56;
    if (strcmp(name, "fork") == 0) return 57;
    if (strcmp(name, "execve") == 0 || strcmp(name, "execute") == 0) return 59;
    if (strcmp(name, "mkdir") == 0) return 83;
    if (strcmp(name, "chmod") == 0) return 90;
    return -1;
}

static const char *sandbox_syscall_name(long syscall_no) {
    switch (syscall_no) {
    case 0: return "read";
    case 1: return "write";
    case 2: return "open";
    case 22: return "pipe";
    case 32: return "dup";
    case 56: return "clone";
    case 57: return "fork";
    case 59: return "execve";
    case 83: return "mkdir";
    case 90: return "chmod";
    default: return NULL;
    }
}

static int sandbox_syscall_arg_count(long syscall_no) {
    switch (syscall_no) {
    case 0:
    case 1:
    case 2:
    case 83:
    case 90:
        return 3;
    case 22:
    case 32:
        return 1;
    case 56:
        return 5;
    case 57:
        return 0;
    case 59:
        return 3;
    default:
        return 0;
    }
}

static unsigned long syscall_arg_value(const struct user_regs_struct *regs, int index) {
    switch (index) {
    case 0: return regs->rdi;
    case 1: return regs->rsi;
    case 2: return regs->rdx;
    case 3: return regs->r10;
    case 4: return regs->r8;
    case 5: return regs->r9;
    default: return 0;
    }
}

static void trim_whitespace(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }

    size_t start = 0;
    while (s[start] != '\0' && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

static void init_deny_list(void) {
    deny_list.rule_num = 0;
    for (int i = 0; i < MAX_RULES; i++) {
        deny_list.rules[i].syscall_no = -1;
        deny_list.rules[i].syscall_name[0] = '\0';
        for (int j = 0; j < MAX_RULE_ARGS; j++) {
            deny_list.rules[i].args[j].specified = 0;
            deny_list.rules[i].args[j].type = DENY_ARG_NONE;
            deny_list.rules[i].args[j].int_value = 0;
            deny_list.rules[i].args[j].str_value[0] = '\0';
        }
    }
}

static int parse_rule_string_value(char **cursor, char *buf, size_t buf_size) {
    size_t len = 0;

    (*cursor)++;
    while (**cursor != '\0' && **cursor != '"') {
        if (len >= buf_size - 1) {
            return 0;
        }
        buf[len++] = **cursor;
        (*cursor)++;
    }

    if (**cursor != '"') {
        return 0;
    }

    buf[len] = '\0';
    (*cursor)++;
    return 1;
}

static int load_deny_rules(const char *rule_path) {
    FILE *fp = fopen(rule_path, "r");
    if (fp == NULL) {
        return 0;
    }

    init_deny_list();
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }
        trim_whitespace(line);
        if (line[0] == '\0') {
            continue;
        }
        if (strncmp(line, "deny:", 5) != 0 || deny_list.rule_num >= MAX_RULES) {
            fclose(fp);
            return 0;
        }

        struct deny_rule *rule = &deny_list.rules[deny_list.rule_num];
        char *cursor = line + 5;
        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            cursor++;
        }

        char *name_start = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (cursor == name_start) {
            fclose(fp);
            return 0;
        }

        char saved_name = *cursor;
        *cursor = '\0';

        rule->syscall_no = sandbox_syscall_number(name_start);
        if (rule->syscall_no == -1) {
            *cursor = saved_name;
            fclose(fp);
            return 0;
        }
        snprintf(rule->syscall_name, sizeof(rule->syscall_name), "%s", sandbox_syscall_name(rule->syscall_no));
        *cursor = saved_name;

        while (*cursor != '\0') {
            while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (*cursor == '\0') {
                break;
            }
            if (strncmp(cursor, "arg", 3) != 0) {
                fclose(fp);
                return 0;
            }

            cursor += 3;
            char *index_start = cursor;
            while (*cursor != '\0' && isdigit((unsigned char)*cursor)) {
                cursor++;
            }
            if (cursor == index_start || *cursor != '=') {
                fclose(fp);
                return 0;
            }

            char saved = *cursor;
            *cursor = '\0';
            int arg_index = atoi(index_start);
            *cursor = saved;
            if (arg_index < 0 || arg_index >= MAX_RULE_ARGS) {
                fclose(fp);
                return 0;
            }

            struct deny_arg *arg = &rule->args[arg_index];
            arg->specified = 1;
            cursor++;

            if (*cursor == '"') {
                arg->type = DENY_ARG_STRING;
                if (!parse_rule_string_value(&cursor, arg->str_value, sizeof(arg->str_value))) {
                    fclose(fp);
                    return 0;
                }
            } else {
                char *value_start = cursor;
                while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
                    cursor++;
                }
                if (cursor == value_start) {
                    fclose(fp);
                    return 0;
                }

                char saved_value = *cursor;
                *cursor = '\0';
                arg->type = DENY_ARG_INT;
                arg->int_value = strtoul(value_start, NULL, 0);
                *cursor = saved_value;
            }
        }

        deny_list.rule_num++;
    }

    fclose(fp);
    return 1;
}

static int read_tracee_string(pid_t pid, unsigned long addr, char *buf, size_t buf_size) {
    if (buf_size == 0) {
        return 0;
    }
    if (addr == 0) {
        buf[0] = '\0';
        return 1;
    }

    size_t offset = 0;
    while (offset < buf_size - 1) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + offset), NULL);
        if (word == -1 && errno != 0) {
            return 0;
        }

        unsigned char *bytes = (unsigned char *)&word;
        for (size_t i = 0; i < sizeof(long) && offset < buf_size - 1; i++) {
            buf[offset++] = (char)bytes[i];
            if (bytes[i] == '\0') {
                return 1;
            }
        }
    }

    buf[buf_size - 1] = '\0';
    return 1;
}

static int read_tracee_bytes(pid_t pid, unsigned long addr, unsigned char *buf, size_t byte_count) {
    size_t offset = 0;

    while (offset < byte_count) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + offset), NULL);
        if (word == -1 && errno != 0) {
            return 0;
        }

        unsigned char *bytes = (unsigned char *)&word;
        size_t chunk = sizeof(long);
        if (chunk > byte_count - offset) {
            chunk = byte_count - offset;
        }
        memcpy(buf + offset, bytes, chunk);
        offset += chunk;
    }

    return 1;
}

static void format_escaped_string(const unsigned char *src, size_t src_len, char *buf, size_t buf_size) {
    static const char hex_chars[] = "0123456789abcdef";
    size_t out = 0;

    if (buf_size == 0) {
        return;
    }

    buf[out++] = '"';
    for (size_t i = 0; i < src_len && out + 1 < buf_size; i++) {
        unsigned char c = src[i];
        const char *escape = NULL;

        switch (c) {
        case '\\':
            escape = "\\\\";
            break;
        case '"':
            escape = "\\\"";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }

        if (escape != NULL) {
            for (int j = 0; escape[j] != '\0' && out + 1 < buf_size; j++) {
                buf[out++] = escape[j];
            }
        } else if (isprint(c)) {
            buf[out++] = (char)c;
        } else if (out + 4 < buf_size) {
            buf[out++] = '\\';
            buf[out++] = 'x';
            buf[out++] = hex_chars[(c >> 4) & 0xf];
            buf[out++] = hex_chars[c & 0xf];
        } else {
            break;
        }
    }

    if (out + 1 < buf_size) {
        buf[out++] = '"';
    } else if (buf_size >= 2) {
        out = buf_size - 2;
        buf[out++] = '"';
    }
    buf[out] = '\0';
}

static int rule_matches(pid_t pid, const struct deny_rule *rule, const struct user_regs_struct *regs) {
    if ((long)rule->syscall_no != (long)regs->orig_rax) {
        return 0;
    }

    for (int i = 0; i < MAX_RULE_ARGS; i++) {
        if (!rule->args[i].specified) {
            continue;
        }

        unsigned long actual = syscall_arg_value(regs, i);
        if (rule->args[i].type == DENY_ARG_INT) {
            if (actual != rule->args[i].int_value) {
                return 0;
            }
        } else if (rule->args[i].type == DENY_ARG_STRING) {
            if ((long)regs->orig_rax == 1 && i == 1) {
                size_t expected_len = strlen(rule->args[i].str_value);
                unsigned long actual_len = syscall_arg_value(regs, 2);
                unsigned char value[MAX_RULE_STR];

                if (actual_len != expected_len || expected_len >= sizeof(value)) {
                    return 0;
                }
                if (!read_tracee_bytes(pid, actual, value, expected_len)) {
                    return 0;
                }
                if (memcmp(value, rule->args[i].str_value, expected_len) != 0) {
                    return 0;
                }
            } else {
                char value[MAX_RULE_STR];
                if (!read_tracee_string(pid, actual, value, sizeof(value))) {
                    return 0;
                }
                if (strcmp(value, rule->args[i].str_value) != 0) {
                    return 0;
                }
            }
        }
    }

    return 1;
}

static int syscall_matches_deny_list(pid_t pid, const struct user_regs_struct *regs) {
    for (int i = 0; i < deny_list.rule_num; i++) {
        if (rule_matches(pid, &deny_list.rules[i], regs)) {
            return i;
        }
    }
    return -1;
}

static void format_blocked_arg(pid_t pid, long syscall_no, int arg_index, unsigned long value,
                               char *buf, size_t buf_size) {
    if (syscall_no == 1 && arg_index == 1) {
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) == 0) {
            size_t byte_count = (size_t)regs.rdx;
            unsigned char raw[MAX_DISPLAY_STR];
            if (byte_count >= sizeof(raw)) {
                byte_count = sizeof(raw) - 1;
            }
            if (read_tracee_bytes(pid, value, raw, byte_count)) {
                format_escaped_string(raw, byte_count, buf, buf_size);
                return;
            }
        }
    }

    if ((syscall_no == 2 && arg_index == 0) ||
        (syscall_no == 59 && arg_index == 0) ||
        (syscall_no == 83 && arg_index == 0)) {
        char str[MAX_RULE_STR];
        if (read_tracee_string(pid, value, str, sizeof(str))) {
            format_escaped_string((unsigned char *)str, strlen(str), buf, buf_size);
            return;
        }
    }

    if ((syscall_no == 0 && arg_index == 1) ||
        (syscall_no == 22 && arg_index == 0) ||
        (syscall_no == 59 && (arg_index == 1 || arg_index == 2))) {
        snprintf(buf, buf_size, "@x%lx", value);
        return;
    }

    snprintf(buf, buf_size, "%lu", value);
}

static void print_blocked_syscall_from_regs(pid_t pid, const struct user_regs_struct *regs) {
    const char *syscall_name = sandbox_syscall_name((long)regs->orig_rax);
    int arg_count = sandbox_syscall_arg_count((long)regs->orig_rax);
    char args[MAX_RULE_ARGS][MAX_DISPLAY_STR];
    char *arg_ptrs[MAX_RULE_ARGS];

    for (int i = 0; i < arg_count; i++) {
        format_blocked_arg(pid, (long)regs->orig_rax, i, syscall_arg_value(regs, i), args[i], sizeof(args[i]));
        arg_ptrs[i] = args[i];
    }

    switch (arg_count) {
    case 0:
        print_blocked_syscall((char *)syscall_name, 0);
        break;
    case 1:
        print_blocked_syscall((char *)syscall_name, 1, arg_ptrs[0]);
        break;
    case 2:
        print_blocked_syscall((char *)syscall_name, 2, arg_ptrs[0], arg_ptrs[1]);
        break;
    case 3:
        print_blocked_syscall((char *)syscall_name, 3, arg_ptrs[0], arg_ptrs[1], arg_ptrs[2]);
        break;
    case 4:
        print_blocked_syscall((char *)syscall_name, 4, arg_ptrs[0], arg_ptrs[1], arg_ptrs[2], arg_ptrs[3]);
        break;
    case 5:
        print_blocked_syscall((char *)syscall_name, 5, arg_ptrs[0], arg_ptrs[1], arg_ptrs[2], arg_ptrs[3], arg_ptrs[4]);
        break;
    case 6:
        print_blocked_syscall((char *)syscall_name, 6, arg_ptrs[0], arg_ptrs[1], arg_ptrs[2], arg_ptrs[3], arg_ptrs[4], arg_ptrs[5]);
        break;
    }
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

static int run_builtin(struct command *cmd, int in_parent) {
    switch (builtin_command_id(cmd)) {
    case 1:
        return run_cd(cmd);
    case 2:
        return in_parent ? 1 : 0;
    case 3:
        return run_env_use(cmd);
    case 4:
        return run_env_exit();
    default:
        return -1;
    }
}

static int find_tracee_index(struct tracee_state *tracees, int tracee_count, pid_t pid) {
    for (int i = 0; i < tracee_count; i++) {
        if (tracees[i].alive && tracees[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static int add_tracee(struct tracee_state *tracees, int *tracee_count, pid_t pid) {
    if (find_tracee_index(tracees, *tracee_count, pid) != -1) {
        return 1;
    }
    if (*tracee_count >= MAX_TRACEES) {
        return 0;
    }

    tracees[*tracee_count].pid = pid;
    tracees[*tracee_count].alive = 1;
    tracees[*tracee_count].in_syscall = 0;
    tracees[*tracee_count].options_set = 0;
    (*tracee_count)++;
    return 1;
}

static int wait_for_sandbox_children(pid_t *pids, int pid_count) {
    struct tracee_state tracees[MAX_TRACEES];
    int tracee_count = 0;
    int remaining = pid_count;
    int blocked = 0;
    long ptrace_options = PTRACE_O_TRACESYSGOOD |
                          PTRACE_O_TRACEFORK |
                          PTRACE_O_TRACEVFORK |
                          PTRACE_O_TRACECLONE;

    for (int i = 0; i < pid_count; i++) {
        if (!add_tracee(tracees, &tracee_count, pids[i])) {
            print_execution_error();
            return 0;
        }
    }

    while (remaining > 0) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, __WALL);
        if (pid == -1) {
            if (errno == EINTR) {
                continue;
            }
            print_execution_error();
            return 0;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            int index = find_tracee_index(tracees, tracee_count, pid);
            if (index != -1) {
                tracees[index].alive = 0;
                remaining--;
            }
            if (blocked) {
                continue;
            }
            if (WIFEXITED(status)) {
                handle_child_status(status);
            } else {
                print_execution_error();
            }
            continue;
        }

        if (WIFSTOPPED(status)) {
            int index = find_tracee_index(tracees, tracee_count, pid);
            if (index == -1) {
                continue;
            }

            if (!tracees[index].options_set) {
                if (ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)ptrace_options) == -1 && errno != ESRCH) {
                    print_execution_error();
                    return 0;
                }
                tracees[index].options_set = 1;
            }

            unsigned int event = (unsigned int)status >> 16;
            if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
                unsigned long new_pid = 0;
                if (ptrace(PTRACE_GETEVENTMSG, pid, NULL, &new_pid) == -1) {
                    print_execution_error();
                    return 0;
                }
                if (!add_tracee(tracees, &tracee_count, (pid_t)new_pid)) {
                    print_execution_error();
                    return 0;
                }
                remaining++;
                if (!blocked && ptrace(PTRACE_SYSCALL, (pid_t)new_pid, NULL, NULL) == -1 && errno != ESRCH) {
                    print_execution_error();
                    return 0;
                }
            }

            if (WSTOPSIG(status) == (SIGTRAP | 0x80)) {
                if (!tracees[index].in_syscall) {
                    struct user_regs_struct regs;
                    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) == -1) {
                        print_execution_error();
                        return 0;
                    }
                    if (!blocked && syscall_matches_deny_list(pid, &regs) != -1) {
                        print_blocked_syscall_from_regs(pid, &regs);
                        blocked = 1;
                        for (int i = 0; i < tracee_count; i++) {
                            if (tracees[i].alive) {
                                kill(tracees[i].pid, SIGKILL);
                            }
                        }
                    }
                }
                tracees[index].in_syscall = !tracees[index].in_syscall;
            }

            if (blocked) {
                continue;
            }

            if (ptrace(PTRACE_SYSCALL, pid, NULL, NULL) == -1 && errno != ESRCH) {
                print_execution_error();
                return 0;
            }
        }
    }

    return 0;
}

int execute(struct job *j, int sandbox_enabled, const char *rule_path) {
    (void)rule_path;
    if (j->cmd_count == 0) {
        return 0;
    }

    if (!validate_execution_plan(j)) {
        print_invalid_syntax();
        return 0;
    }

    if (j->cmd_count == 1) {
        int builtin_result = run_builtin(&j->cmds[0], 1);
        if (builtin_result != -1) {
            return builtin_result;
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
            if (sandbox_enabled) {
                if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
                    _exit(EXIT_EXEC_ERROR);
                }
                raise(SIGSTOP);
            }
            if (!setup_child_io(j, i, pipes)) {
                _exit(EXIT_EXEC_ERROR);
            }
            close_all_pipes(pipes, pipe_count);
            int builtin_result = run_builtin(&j->cmds[i], 0);
            if (builtin_result != -1) {
                _exit(builtin_result == 0 ? 0 : EXIT_EXEC_ERROR);
            }
            run_external_command(&j->cmds[i]);
        }
    }

    close_all_pipes(pipes, pipe_count);

    if (sandbox_enabled) {
        return wait_for_sandbox_children(pids, j->cmd_count);
    }

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

    init_deny_list();

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
        if (!tokenizer(input)) {
            print_invalid_syntax();
            continue;
        }
        int sandbox_enabled = 0;
        char *sandbox_rule_path = NULL;
        if (ntok >= 1 && tokens[0].type == WORD && strcmp(tokens[0].value, "sandbox") == 0) {
            if (ntok < 3 || tokens[1].type != WORD) {
                print_invalid_syntax();
                free_tokens();
                continue;
            }
            sandbox_enabled = 1;
            sandbox_rule_path = tokens[1].value;
            if (!load_deny_rules(sandbox_rule_path)) {
                print_execution_error();
                free_tokens();
                continue;
            }
            for (int i = 2; i < ntok; i++) {
                tokens[i - 2] = tokens[i];
            }
            ntok -= 2;
        } else {
            init_deny_list();
        }
        struct job j;
        if (!parser(&j)) {
            print_invalid_syntax();
        } else {
            if (execute(&j, sandbox_enabled, sandbox_rule_path)) {
                free_tokens();
                break;
            }
        }


        free_tokens();
    }
    free(input);
    return 0;
}
