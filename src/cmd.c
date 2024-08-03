// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cmd.h"
#include "utils.h"

#define READ 0
#define WRITE 1

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	// If dir is NULL, change to the HOME directory
	if (dir == NULL) {
		// Attempt to change to the HOME directory
		if (chdir(getenv("HOME")) != 0) {
			// Return false if unable to change directory to HOME
			return false;
		}
	} else {
		// Attempt to change to the provided directory
		if (chdir(dir->string) != 0) {
			// Return false if unable to change directory to the provided path
			return false;
		}
	}

	// Return true upon successful directory change
	return true;
}
/**
 * Internal exit/quit command.
 */
static int shell_exit(int status)
{
	// Exits the shell with the given status code
	exit(status);
}

char *getFile(word_t *w)
{
	// Allocate memory for the file path or name (100 characters)
	char *file = calloc(100, sizeof(char));

	// Check if memory allocation was successful
	DIE(file == NULL, "malloc");

	// Iterate through the linked list of words
	while (w != NULL) {
		// If the word needs expansion (is an environment variable)
		if (w->expand == true) {
			// Get the value of the environment variable
			char *valoare = getenv(w->string);

			// If the environment variable has a value, concatenate it to the file
			if (valoare != NULL) {
				strcat(file, valoare);
			} else {
				// If the environment variable is not set, concatenate an empty string
				strcat(file, "");
			}
		} else {
			// If the word doesn't need expansion, concatenate it to the file
			strcat(file, w->string);
		}

		// Move to the next word in the linked list
		w = w->next_part;
	}

	// Return the constructed file path or name
	return file;
}

int *saveAllAndSwitch(simple_command_t *s)
{
	int saved_stdin;
	int saved_stdout;
	int saved_stderr;

	int fd_in;
	int fd_out;
	int fd_err;

	// Save stdin if an input file is provided
	if (s->in != NULL) {
		saved_stdin = dup(STDIN_FILENO);
		char *file = getFile(s->in);

		fd_in = open(file, O_RDONLY);
		dup2(fd_in, STDIN_FILENO);
	}

	// Save stdout if an output file is provided
	if (s->out != NULL) {
		saved_stdout = dup(STDOUT_FILENO);

		// Determine how to open the output file based on flags
		if (s->io_flags == IO_OUT_APPEND || (s->err != NULL && strcmp(s->out->string, s->err->string) == 0)) {
			char *file = getFile(s->out);

			fd_out = open(file, O_WRONLY | O_CREAT | O_APPEND, 0644);
			dup2(fd_out, STDOUT_FILENO);
		} else {
			char *file = getFile(s->out);

			fd_out = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(fd_out, STDOUT_FILENO);
		}
	}

	// Save stderr if an error file is provided
	if (s->err != NULL) {
		saved_stderr = dup(STDERR_FILENO);

		// Determine how to open the error file based on flags
		if (s->io_flags == IO_ERR_APPEND) {
			char *file = getFile(s->err);

			fd_err = open(file, O_WRONLY | O_CREAT | O_APPEND, 0644);
			dup2(fd_err, STDERR_FILENO);
		} else {
			char *file = getFile(s->err);

			fd_err = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			dup2(fd_err, STDERR_FILENO);
		}
	}

	// Allocate memory to store the saved file descriptors
	int *saved = malloc(6 * sizeof(int));

	DIE(saved == NULL, "malloc");

	// Store the saved file descriptors in the allocated memory
	saved[0] = saved_stdin;
	saved[1] = saved_stdout;
	saved[2] = saved_stderr;
	saved[3] = fd_in;
	saved[4] = fd_out;
	saved[5] = fd_err;

	// Return the array of saved file descriptors
	return saved;
}

void switchAndClose(int *saved, simple_command_t *s)
{
	// Restore stdin if an input file was provided
	if (s->in != NULL)
		dup2(saved[0], STDIN_FILENO);

	// Restore stdout if an output file was provided
	if (s->out != NULL)
		dup2(saved[1], STDOUT_FILENO);

	// Restore stderr if an error file was provided
	if (s->err != NULL)
		dup2(saved[2], STDERR_FILENO);

	// Close file descriptors opened during redirection
	close(saved[3]);
	close(saved[4]);
	close(saved[5]);
	// Free the memory allocated for the array of saved file descriptors
	free(saved);
}

char **getParameters(simple_command_t *s)
{
	// Allocate memory for the argument list
	char **argv = calloc(10, sizeof(char *));

	// Set the first argument as the command's verb
	argv[0] = (char *)s->verb->string;

	int i = 1;
	word_t *param = s->params;

	while (param != NULL) {
		// Retrieve each parameter
		word_t *var = param;

		// Allocate memory for each argument's value
		char *value = calloc(100, sizeof(char));

		while (var != NULL) {
			if (var->expand == true) {
				// Get value from the environment for the current variable
				char *valoare = getenv(var->string);

				if (valoare != NULL)
					strcat(value, valoare); // Concatenate the environment value to the current argument's value
				else
					strcat(value, ""); // Concatenate an empty string if environment value is NULL
			} else {
				strcat(value, var->string); // Concatenate directly the variable's value to the current argument's value
			}
			var = var->next_part; // Move to the next part of the current variable
		}
		argv[i] = value; // Store the argument's value in the argument list
		param = param->next_word; // Move to the next parameter
		i++;
	}

	argv[i] = NULL; // Null-terminate the argument list
	return argv; // Return the argument list
}


/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	// Sanity checks for the command structure
	if (s == NULL || s->verb == NULL)
		return 1; // Indicate an error if essential elements are missing

	// Handling variable assignment in the form of 'var=value'
	if (s->verb->next_part != NULL) {
		if (strcmp(s->verb->next_part->string, "=") == 0) {
			// Extracting the variable name and its value from the command structure
			word_t *var = s->verb->next_part->next_part;

			char value[100] = {0}; // Initializing a buffer for the value

			// Construct the value for the variable assignment
			while (var != NULL) {
				if (var->expand == true) {
					// If the word needs expansion (is an environment variable), fetch its value
					char *valoare = getenv(var->string);

					if (valoare != NULL) {
						// If the environment variable has a value, append it to 'value'
						strcat(value, valoare);
					} else {
						// If the environment variable is not set, append an empty string
						strcat(value, "");
					}
				} else {
					// If the word doesn't need expansion, append it to 'value'
					strcat(value, var->string);
				}
				var = var->next_part; // Move to the next word in the command
			}

			// Set the constructed value to the variable name in the environment
			setenv(s->verb->string, value, 1);
			return 0; // Return success status for the variable assignment
		}
	}

	// Handling built-in commands like 'exit', 'quit', 'true', 'false'
	if (strcmp(s->verb->string, "exit") == 0 || strcmp(s->verb->string, "quit") == 0)
		return shell_exit(0); // Perform shell exit
	if (strcmp(s->verb->string, "true") == 0)
		return 0; // Return success status for 'true'
	if (strcmp(s->verb->string, "false") == 0)
		return 1; // Return failure status for 'false'

	pid_t pid = fork();

	if (pid == 0) {
		if (s->params != NULL) {
			// Handling commands with parameters

			// Constructing the argument list for the command
			char **argv = getParameters(s);

			int *saved = saveAllAndSwitch(s);

			// Execute 'cd' command if requested
			if (strcmp(s->verb->string, "cd") == 0) {
				shell_cd(s->params);
			} else {
				// Execute the command using 'execvp' and handle execution failure
				if (execvp(s->verb->string, argv) == -1) {
					printf("Execution failed for '%s'\n", s->verb->string);
					shell_exit(1);
				}
			}
			switchAndClose(saved, s);
		} else {
			// Handling commands without parameters
			// Create an argument list for the command
			char *noParameters[2] = {(char *)s->verb->string, NULL};

			// Save file descriptors and perform I/O redirection
			int *saved = saveAllAndSwitch(s);

			// Execute 'cd' command if requested
			if (strcmp(s->verb->string, "cd") == 0) {
				shell_cd(s->params);
			} else {
				// Execute the command using 'execvp' and handle execution failure
				if (execvp(s->verb->string, noParameters) == -1) {
					printf("Execution failed for '%s'\n", s->verb->string);
					shell_exit(1);
				}
			}

			// Restore file descriptors after execution
			switchAndClose(saved, s);
		}

		shell_exit(0); // Exit the child process
	} else if (pid > 0) {
		// Parent process
		int status;

		waitpid(pid, &status, 0); // Wait for the child process to complete
		bool cd_return = true;

		// Handling 'cd' command separately if present
		if (strcmp(s->verb->string, "cd") == 0)
			cd_return = shell_cd(s->params);

		// Returning exit status based on 'cd' command return or child process status
		if (!cd_return)
			return 1; // Return failure status if 'cd' command fails
		return status; // Return the exit status of the child process
	}
	// Error handling for fork failure
	perror("fork");
	return -1; // Indicate an error
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level, command_t *father)
{
	pid_t pid1, pid2;

	int status1, status2;

	// Fork to execute cmd1
	pid1 = fork();
	if (pid1 == 0) {
		// Child process for cmd1
		status1 = parse_command(cmd1, level, father); // Execute cmd1
		shell_exit(status1); // Exit the child process with cmd1's status
	} else if (pid1 > 0) {
		// Parent process after forking cmd1
		// Fork to execute cmd2
		pid2 = fork();
		if (pid2 == 0) {
			// Child process for cmd2
			status2 = parse_command(cmd2, level, father); // Execute cmd2
			shell_exit(status2); // Exit the child process with cmd2's status
		} else if (pid2 > 0) {
			// Parent process after forking cmd2
			// Wait for both child processes to complete
			waitpid(pid1, &status1, 0);
			waitpid(pid2, &status2, 0);
		}
	}
	return true; // Placeholder return value, replace with actual logic
}


/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level, command_t *father)
{
	int pipefd[2];

	pipe(pipefd); // Create a pipe

	pid_t pid1, pid2;

	int status1, status2;

	pid1 = fork();

	if (pid1 == 0) {
		// Child process for cmd1
		close(pipefd[READ]); // Close unused read end of the pipe
		dup2(pipefd[WRITE], STDOUT_FILENO); // Redirect stdout to the pipe write end
		close(pipefd[WRITE]); // Close the write end of the pipe
		status1 = parse_command(cmd1, level, father); // Execute cmd1
		shell_exit(status1); // Exit the child process with cmd1's status
	} else if (pid1 > 0) {
		// Parent process after forking cmd1
		pid2 = fork();
		if (pid2 == 0) {
			// Child process for cmd2
			close(pipefd[WRITE]); // Close unused write end of the pipe
			dup2(pipefd[READ], STDIN_FILENO); // Redirect stdin to the pipe read end
			close(pipefd[READ]); // Close the read end of the pipe
			status2 = parse_command(cmd2, level, father); // Execute cmd2
			shell_exit(status2); // Exit the child process with cmd2's status
		} else if (pid2 > 0) {
			// Parent process after forking cmd2
			close(pipefd[READ]); // Close both ends of the pipe in the parent
			close(pipefd[WRITE]);
			waitpid(pid1, &status1, 0); // Wait for cmd1 to finish
			waitpid(pid2, &status2, 0); // Wait for cmd2 to finish
		}
	}

	return status2; // Return the exit status of cmd2
}


int Conditional_Zero(command_t *c, int level, command_t *father)
{
	// Check if there's no subcommand
	if (c->scmd == NULL) {
		// Execute cmd1
		int status = parse_command(c->cmd1, level, father);

		// Check if cmd1 is a 'cd' command and change directory if applicable
		if (c && c->cmd1 && c->cmd1->scmd && c->cmd1->scmd->verb != NULL &&
			strcmp(c->cmd1->scmd->verb->string, "cd") == 0)
			shell_cd(c->cmd1->scmd->params);

		// Execute cmd2 if cmd1 succeeded
		if (status == 0)
			status = parse_command(c->cmd2, level, father);
		return status;
	}

	// If there's a subcommand, fork a child process
	pid_t pid = fork();

	if (pid == 0) {
		// Child process
		int status = parse_command(c->cmd1, level, father);

		// Check if cmd1 is a 'cd' command and change directory if applicable
		if (c && c->cmd1 && c->cmd1->scmd && c->cmd1->scmd->verb != NULL &&
			strcmp(c->cmd1->scmd->verb->string, "cd") == 0)
			shell_cd(c->cmd1->scmd->params);

		// Execute cmd2 if cmd1 succeeded and exit with cmd2's status
		if (status == 0)
			status = parse_command(c->cmd2, level, father);
		shell_exit(status);
	} else if (pid > 0) {
		// Parent process
		int status;

		waitpid(pid, &status, 0);

		// Check if cmd1 is a 'cd' command and change directory if applicable
		if (c && c->cmd1 && c->cmd1->scmd && c->cmd1->scmd->verb != NULL &&
			strcmp(c->cmd1->scmd->verb->string, "cd") == 0) {
			shell_cd(c->cmd1->scmd->params);
		}

		return status; // Return the status of the child process
	}

	return 0; // Default return value
}


int Conditional_NZero(command_t *c, int level, command_t *father)
{
	// Check if there's no subcommand
	if (c->scmd == NULL) {
		// Execute cmd1
		int status = parse_command(c->cmd1, level, father);

		// Check if cmd1 is a 'cd' command and change directory if applicable
		if (c && c->cmd1 && c->cmd1->scmd && c->cmd1->scmd->verb != NULL &&
			strcmp(c->cmd1->scmd->verb->string, "cd") == 0)
			shell_cd(c->cmd1->scmd->params);

		// Execute cmd2 if cmd1 failed
		if (status != 0)
			status = parse_command(c->cmd2, level, father);
		return status;
	}

	// If there's a subcommand, fork a child process
	pid_t pid = fork();

	if (pid == 0) {
		// Child process
		int status = parse_command(c->cmd1, level, father);

		// Check if cmd1 is a 'cd' command and change directory if applicable
		if (c && c->cmd1 && c->cmd1->scmd && c->cmd1->scmd->verb != NULL &&
			strcmp(c->cmd1->scmd->verb->string, "cd") == 0)
			shell_cd(c->cmd1->scmd->params);

		// Execute cmd2 if cmd1 failed and exit with cmd2's status
		if (status != 0)
			status = parse_command(c->cmd2, level, father);
		shell_exit(status);
	} else if (pid > 0) {
		// Parent process
		int status;

		waitpid(pid, &status, 0);
		return status; // Return the status of the child process
	}

	return 0; // Default return value
}



/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	if (c == NULL)
		return 0; // If the command is NULL, return 0 (default)

	if (c->op == OP_NONE)
		return parse_simple(c->scmd, level, father); // Execute a simple command

	switch (c->op) {
	case OP_SEQUENTIAL:
		parse_command(c->cmd1, level, father); // Execute first command
		parse_command(c->cmd2, level, father); // Execute second command
		break;

	case OP_PARALLEL:
		run_in_parallel(c->cmd1, c->cmd2, level, father); // Execute commands in parallel
		break;

	case OP_CONDITIONAL_NZERO:
		return Conditional_NZero(c, level, father); // Execute based on conditional non-zero result

	case OP_CONDITIONAL_ZERO:
		return Conditional_Zero(c, level, father); // Execute based on conditional zero result

	case OP_PIPE:
		return run_on_pipe(c->cmd1, c->cmd2, level, father); // Execute commands connected via a pipe

	default:
		return SHELL_EXIT; // If the operation type is not recognized, exit
	}

	return 0; // Default return value
}

