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

void program_exec(char **args, char *inbuf, char *outbuf);
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
  printf("An error happens when changing the working directory. errorno: %d.\n", errno);
  return 0;
}

/* Prints the current working directory to standard output */
int cmd_pwd(struct tokens *tokens) {
  char *path = getcwd(NULL, 0);
  if (!path) {
    printf("An error happens when getting the current path.\n");
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
  }
}

void exec_with_pathres(char **args) {
  char pathbuf[1024];
  char *path = args[0];

  if (execv(path, args) == -1 && errno == ENOENT) {
    int pathlen = strlen(path);
    /* Split the PATH string by colon */
    char *envpath = getenv("PATH");
    char *p = strtok(envpath, ":");

    while (p) {
      /* Path too long */
      if (pathlen + strlen(p) >= 1023) {
        printf("The path to the executable is too long.\n");
        return;
      }
      
      strcpy(pathbuf, p);
      strcat(pathbuf, "/");
      strcat(pathbuf, path);
      args[0] = pathbuf;

      if (execv(pathbuf, args) == -1 && errno != ENOENT) 
        break;
      
      p = strtok(NULL, ":");
    }
  }
  command_not_found(path);
  exit(1);
}

/* Try to execute the program provided by the user */
void program_exec(char **args, char *inbuf, char *outbuf) {
  /* Forks a new process */
  pid_t pid = fork();
  int status;
  /* Parent process */
  if (pid > 0) {
    wait(&status);
    // if (status)
    //   printf("Program exits with return value %d.\n", WEXITSTATUS(status));
  } else if (pid == 0) {
    /* Child process */
    if (input_redirect)
    {
        int fd0 = open(inbuf, O_RDONLY);
        if (fd0 == -1) {
          printf("Cannot open file: %d\n", errno);
          exit(1);
        }
        /* Input redirect for child process */
        dup2(fd0, STDIN_FILENO);
        close(fd0);
    }

    if (output_redirect)
    {
        int fd1 = creat(outbuf, (O_RDWR | O_CREAT | O_TRUNC));
        if (fd1 == -1) {
          printf("Cannot create file: %d\n", errno);
          exit(1);
        }
        /* Output redirect for child process */
        dup2(fd1, STDOUT_FILENO);
        close(fd1);
    }
    exec_with_pathres(args);
  } else {
    printf("Failed to create new process.\n");
  }
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

    /* input and output filename buffer */
    char inbuf[128];
    char outbuf[128];

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      size_t token_length = tokens_get_length(tokens);
      /* Get the argv and path for the executable */
      char **args = (char **)malloc(sizeof(char *) * (token_length + 1));

      args[0] = tokens_get_token(tokens, 0);
      for (unsigned int i = 1; i < token_length; i++) {
        /* Search for the redirection symbol '>' '<' */
        args[i] = tokens_get_token(tokens, i);
        if (!strcmp(args[i - 1], "<")) {
          args[i - 1] = NULL;
          strcpy(inbuf, args[i]);
          input_redirect = 1;
        } else if (!strcmp(args[i - 1], ">")) {
          args[i - 1] = NULL;
          strcpy(outbuf, args[i]);
          output_redirect = 1;
        }
      }

      args[token_length] = NULL;
      program_exec(args, inbuf, outbuf);
      free(args);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
