#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

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

#define MAX_ARGS 32
#define MAX_CMDS 16
#define MAX_INPUT 1024
#define MAX_TOKENS 128

char input[MAX_INPUT];

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
    int background;
};

struct token {
    enum token_type {
        WORD,
        PIPE,
        REDIRECT_IN,
        REDIRECT_OUT,
        REDIRECT_APPEND,
        BACKGROUND
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
        } else if(line[i] == '&') {
            tokens[ntok].type = BACKGROUND;
            tokens[ntok].value = NULL;
            i++;
        } else {
            int j = i;
            while(j + 1 < len && !isspace(line[j + 1]) && line[j + 1] != '|' &&
                line[j + 1] != '>' && line[j + 1] != '<' && line[j + 1] != '&') {
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
    j->background = 0;
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
        case BACKGROUND:
            if (i != ntok - 1 || command_is_empty(&current) || j->background) {
                return 0;
            }
            j->background = 1;
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

void execute(struct job *j) {
    return;
}

int main() {
    while(1) {
        print_prompt();
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        tokenizer(input);
        struct job j;
        if (!parser(&j)) {
            print_invalid_syntax();
        }
        execute(&j);


        free_tokens();
    }
    return 0;
}
