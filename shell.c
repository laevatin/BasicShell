#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

#define PIPE_READ 0
#define PIPE_WRITE 1

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int input_redirect = 0;
int output_redirect = 0;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);

void program_exec(char **args, int pipein, int pipeout);
void piped_exec(struct tokens *tokens);
void command_not_found(const char *cmd);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_cd, "cd", "changes the working directory to the given directory"},
  {cmd_pwd, "pwd", "prints the current working directory to standard output"},
};

/* input and output filename buffer */
char inbuf[128];
char outbuf[128];

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* Changes the working directory to the given directory */
int cmd_cd(struct tokens *tokens) {
  char *path = tokens_get_token(tokens, 1);
  if (chdir(path) != -1) {
    return 1;
  }
  printf("%s: %s.\n", path, strerror(errno));
  return 0;
}

/* Prints the current working directory to standard output */
int cmd_pwd(struct tokens *tokens) {
  char *path = getcwd(NULL, 0);
  if (!path) {
    printf("error: %s.\n", strerror(errno));
    return 0;
  }
  printf("%s\n", path);
  free(path);
  return 1;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);

    /* Ignore the signals should not be received by the shell */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCONT, SIG_IGN);    
  }
}

void exec_with_pathres(char **args) {
  /* Split the PATH string by colon */
  char *envpath = getenv("PATH");
  size_t envlen = strlen(envpath);
  char *pathbuf = (char *)malloc(envlen + 1);
  char *envbuf = (char *)malloc(envlen + 1);
  char *path = args[0];

  /* Copy the envpath since strtok changes the actual PATH */
  strcpy(envbuf, envpath);

  if (execv(path, args) == -1 && errno == ENOENT) {
    char *p = strtok(envbuf, ":");

    while (p) {
      strcpy(pathbuf, p);
      strcat(pathbuf, "/");
      strcat(pathbuf, path);
      args[0] = pathbuf;

      if (execv(args[0], args) == -1 && errno != ENOENT) 
        break;
      
      p = strtok(NULL, ":");
    }
  }

  free(pathbuf);
  free(envbuf);
  command_not_found(path);
  exit(EXIT_FAILURE);
}

/* Try to execute the program provided by the user */
void program_exec(char **args, int pipein, int pipeout) {
  /* Forks a new process */
  pid_t pid = fork();
  int status;

  /* Parent process */
  if (pid > 0) {
    /* Ignore the SIGTTOU */
    signal(SIGTTOU, SIG_IGN);

    /* Set child to its own process group */
    if (setpgid(pid, pid) < 0) {
      perror("setpgid failed");
    }

    /* Put child process to foreground */
    if (tcsetpgrp(STDIN_FILENO, pid) < 0) {
      perror("tcsetpgrp failed");
    }

    /* Wait the child process to finish */
    if (wait(&status) == -1) {
      perror("wait failed");
    }
    printf("status: %d\n", status);

    /* Put parent process to foreground */
    if (tcsetpgrp(STDIN_FILENO, shell_pgid) < 0) {
      perror("tcsetpgrp failed");
    }
    /* Reset SIGTTOU */
    signal(SIGTTOU, SIG_DFL);

  } else if (pid == 0) {
    /* Child process */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCONT, SIG_DFL);
    if (pipein != -1) {
      /* There is pipe */
      if (dup2(pipein, STDIN_FILENO) == -1) {
        perror("dup2 error");
        exit(EXIT_FAILURE);
      }

      if (dup2(pipeout, STDOUT_FILENO) == -1) {
        perror("dup2 error");
        exit(EXIT_FAILURE);
      }
    }

    if (input_redirect) {
        int fd0 = open(inbuf, O_RDONLY);
        if (fd0 == -1) {
          perror("Cannot open file");
          exit(EXIT_FAILURE);
        }

        /* Input redirect for child process */
        if (dup2(fd0, STDIN_FILENO) == -1) {
          perror("dup2 error");
          exit(EXIT_FAILURE);
        }
          
        close(fd0);
    }

    if (output_redirect) {
        int fd1 = creat(outbuf, (O_RDWR | O_CREAT | O_TRUNC));
        if (fd1 == -1) {
          perror("Cannot create file");
          exit(EXIT_FAILURE);
        }

        /* Output redirect for child process */
        if (dup2(fd1, STDOUT_FILENO) == -1) {
          perror("dup2 error");
          exit(EXIT_FAILURE);
        }

        close(fd1);
    }

    exec_with_pathres(args);
  } else {
    printf("Failed to create new process: %s.\n", strerror(errno));
  }
}

/* Execute the programs with pipe */
void piped_exec(struct tokens *tokens) {
  int token_len = tokens_get_length(tokens);
  char **args = (char **)malloc(sizeof(char *) * (token_len + 1));
  int j = 1;
  int prevpipe[2] = {-1, -1};
  int curpipe[2] = {-1, -1};

  args[0] = tokens_get_token(tokens, 0);
  char *prevtok = args[0];

  for (int i = 1; i < token_len; i++) {
    char *curtok = tokens_get_token(tokens, i);
    args[j++] = curtok;

    /* j is guaranteed to be greater than 1 */
    if (!strcmp(prevtok, "|")) {
      /* Change prevtok in args to NULL */
      args[j - 2] = NULL;
      // printf("exec: %s, %s\n", args[0], args[1]);

      /* Check pipe state */
      if (pipe(curpipe) == -1) {
        perror("pipe cannot be created");
        return;
      }

      /* If the program is the first one, it reads from STDIN */
      if (prevpipe[PIPE_READ] == -1)
        program_exec(args, STDIN_FILENO, curpipe[PIPE_WRITE]);
      else 
        program_exec(args, prevpipe[PIPE_READ], curpipe[PIPE_WRITE]);

      /* Close the used pipes */
      if (prevpipe[PIPE_READ] != -1) {
        close(prevpipe[PIPE_READ]);
        close(prevpipe[PIPE_WRITE]);
      }

      memcpy(prevpipe, curpipe, sizeof(prevpipe));
      /* Reset the index for program arguments */
      args[0] = curtok;
      j = 1;

      /* Reset the redirection flags */
      input_redirect = 0;
      output_redirect = 0;
    } else if (!strcmp(prevtok, "<")) {
      args[j - 2] = NULL;
      strcpy(inbuf, curtok);
      input_redirect = 1;
    } else if (!strcmp(prevtok, ">")) {
      args[j - 2] = NULL;
      strcpy(outbuf, curtok);
      output_redirect = 1;
    }

    prevtok = curtok;
  }

  args[j] = NULL;
  // printf("exec: %s, %s, %d, %d\n", args[0], args[1], curpipe[PIPE_READ], STDOUT_FILENO);
  /* The last one output to STDOUT if no redirection */
  program_exec(args, curpipe[PIPE_READ], STDOUT_FILENO);

  /* Close the used pipes */
  if (prevpipe[PIPE_READ] != -1) {
    close(prevpipe[PIPE_READ]);
    close(prevpipe[PIPE_WRITE]);
  }

  free(args);
}

void command_not_found(const char *cmd) {
  printf("%s: command not found.\n", cmd);
}

int main(unused int argc, unused char *argv[]) {
  init_shell();
  
  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    /* Clear the input and output redirection flag */
    input_redirect = 0;
    output_redirect = 0;

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else if (tokens_get_token(tokens, 0)) {
      /* Skip empty input */
      piped_exec(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
