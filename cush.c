/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech. Augmented to use posix_spawn in Fall 2021.
 */

// Enable GNU specific extensions
#define _GNU_SOURCE    1
// Define the maximum number of processes that can be managed by the shell
#define MAX_PROCESSES 10

// Include standard libraries for IO operations, process control, memory allocation, string manipulation, terminal control, process waiting, assertions, spawning processes, and file control
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <spawn.h>
#include <fcntl.h>


#include <signal.h>


#pragma GCC diagnostic ignored "-Wunused-function"


#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

// Declaration of function to handle status changes in child processes
static void handle_child_status(pid_t pid, int status);
extern char **environ;

// Function to print usage information for the shell
static void usage(char *progname)
{
    printf("Usage: %s -h\n"
           " -h            print this help\n",
           progname);
    exit(EXIT_SUCCESS);
}

// Function to build and return the shell prompt string
static char *build_prompt(void)
{
   
    return strdup("cush> ");
}

// Enumeration defining possible statuses for jobs managed by the shell
enum job_status {
    FOREGROUND,     // Job running in foreground, interactively
    BACKGROUND,     // Job running in background
    STOPPED,        // Job paused by SIGSTOP
    NEEDSTERMINAL,  // Background job requiring terminal access
    TERMINATED,     // Job has been terminated
};

// Structure defining a job, including its command pipeline and status
struct job {
    struct list_elem elem;              // Element for linking in the job list
    struct ast_pipeline *pipe;          // Command pipeline executed by this job
    int     jid;                        
    enum job_status status;             // Current status of the job
    int  num_processes_alive;           
    struct termios saved_tty_state;     // Terminal state when job was last in foreground
    pid_t pids[MAX_PROCESSES];          // Array of process IDs belonging to this job
    pid_t gpid;                         // Group ID for the job's processes
    bool saved_tty_state_flag;         
};

// Define the maximum number of jobs and initialize job management structures
#define MAXJOBS (1<<16)                  // Maximum number of jobs manageable
static struct list job_list;             // Linked list of jobs
static struct job *jid2job[MAXJOBS];     // Array for quick job ID to job lookup

// Function to retrieve a job by its ID
static struct job *get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];
    return NULL; // Return null if job ID is invalid or job doesn't exist
}

// Function to add a new job to the job management system
static struct job *add_job(struct ast_pipeline *pipe)
{
    struct job *job = malloc(sizeof *job); 
    job->pipe = pipe;                      // Set the command pipeline for the job
    job->num_processes_alive = 0;          // Initialize the process count to 0
    list_push_back(&job_list, &job->elem); // Add the job to the job list
    
    // Assign a unique job ID
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job; // Return the newly added job
        }
    }

    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort(); // Terminate if the job limit is exceeded
    return NULL; 
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
/* Function to remove a job from the job list and free its resources */
static void delete_job(struct job *job)
{
    // Iterate through the job list to find the job to delete
    struct list_elem *k;
    for (k = list_begin(&job_list); k != list_end(&job_list); k = list_next(k)) {
        struct job *jobs = list_entry(k, struct job, elem);
        if(jobs->jid == job->jid)
        {
            // Remove the job from the list and break out of the loop
            list_remove(k);
            break;
        }
    }
    // Ensure the job ID is valid before proceeding with deletion
    int jid = job->jid;
    assert(jid != -1); // Assert job ID is not invalid
    // Clear the job entry in the jid2job mapping and free job resources
    jid2job[jid]->jid = -1; // Mark the job ID as deleted
    jid2job[jid] = NULL; // Remove the job from the job ID mapping
    ast_pipeline_free(job->pipe); // Free the AST pipeline associated with the job
    free(job); // Free the job structure itself
}

/* Function to return a human-readable string representing a job's status */
static const char *get_status(enum job_status status)
{
    // Switch on the job status to return the corresponding string
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    case TERMINATED:
        return "Terminated";
    default:
        return "Unknown"; // Default case for unrecognized status
    }
}

/* Function to print the command line associated with a job's pipeline */
static void print_cmdline(struct ast_pipeline *pipeline)
{
    // Iterate through each command in the pipeline
    struct list_elem *e = list_begin(&pipeline->commands); 
    for (; e != list_end(&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        // Print a pipe symbol between commands
        if (e != list_begin(&pipeline->commands)) printf("| ");
        // Print each argument of the command
        for (char **p = cmd->argv; *p; p++) printf("%s ", *p);
    }
}

/* Function to print detailed information about a job */
static void print_job(struct job *job)
{
    // Print the job ID, status, and the command line of the pipeline
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/* SIGCHLD handler to manage child processes that have exited or changed state */
static void sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    // Assert signal is SIGCHLD to catch programming errors
    assert(sig == SIGCHLD);

    // Use waitpid in a loop to reap all exited or stopped children
    pid_t child;
    int status;
    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        // Call handle_child_status to process the exit or stop status of the child
        handle_child_status(child, status);
    }
}


/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));
    //printf("Job Status: %s\n", get_status(job->status));
    //printf("Number of Processes Alive: %d\n", job->num_processes_alive);

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;
        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */

    //struct job *job = NULL; //= determine_job_from_pid(pid);
    //fprintf(stderr, "PID to find:  %d\n", pid);
    struct list_elem *k;
    struct job *job = NULL;

    // Find the job associated with the provided pid ------------------------------------------------------------------------------
    for (k = list_begin(&job_list); k != list_end(&job_list); k = list_next(k)) {
        struct job *jobs = list_entry(k, struct job, elem);
        for (int i = 0; i < sizeof(jobs->pids); i++) {
            //fprintf(stderr, "pids found in the job: %d\n", jobs->pids[i]);
            if (pid == jobs->pids[i]) {
                //fprintf(stderr, "PID in child signal handler 01: %d\n", pid);
                job = jobs;
                break;  // Exit the inner loop if pid is found
            }
        }
        if (job != NULL) {
            break;  // Exit the outer loop if job is found
        }
    }

    if (job == NULL) {
        printf("Job not found for PID %d\n", pid);
        return;  // Return if job is not found
    }

    // Update job status based on the child status
    /**if (WIFEXITED(status)) {
        job->num_processes_alive--;
        if (job->num_processes_alive == 0) {
            job->status = STOPPED;
        }
    } else if (WIFSIGNALED(status)) {
        // Handle signal termination
    } else if (WIFSTOPPED(status)) {
        // Handle suspension
    } else if (WIFCONTINUED(status)) {
        // Handle continued execution
    }**/

    // This is the terminate signal for ctrl-c ------------------------------------------------------------------------------
    // this currently doesnt work, the signal isnt being caught ------------------------------------------------------------------------------
    // we need to talk to the ta abt this ------------------------------------------------------------------------------
    if(WIFSIGNALED(status))
    {
        // so all this does is chanage the job status,
        // the job actually gets deleted when jobs gets called using delete_job
        // remove job form jobs list
        printf("terminated gpid: %d\n", job->gpid);
        job->status = TERMINATED;
    }

    // gets called when child finishes executing, it works ------------------------------------------------------------------------------
    else if(WIFEXITED(status))
    {
        //fprintf(stderr, "PID in child signal handler 02: %d\n", pid);
        if(job->status == BACKGROUND)
        {
            printf("\n[%d]        Done\n", job->jid);
        }
        job->num_processes_alive--;
        if(job->num_processes_alive == 0 && job->status == FOREGROUND)
        {
            job->status = 0;
            termstate_sample();
        }
    }
    // ctrl-z ------------------------------------------------------------------------------
    else if(WIFSTOPPED(status))
    {

        printf("stop signal");
        //fprintf(stderr, "child stopped");
        
        //printf("[%d]   Stopped         %s\n", job->jid, job->pipe);

        struct list_elem *c; 
        //for (e = list_begin(&job->pipe); e != list_end(&job->pipe); e = list_next(e)) {
            struct ast_pipeline *pipeline = job->pipe;
            for (c = list_begin(&pipeline->commands); c != list_end(&pipeline->commands); c = list_next(c)) {
                struct ast_command *command = list_entry(c, struct ast_command, elem);
                printf("\n[%d]   Stopped         (%s %s)\n", job->jid, command->argv[0], command->argv[1]);
            }
        //}

        job->status = STOPPED;
        // save the job state ------------------------------------------------------------------------------
        termstate_save(&job->saved_tty_state);
        // flag for if we saved a state or not ------------------------------------------------------------------------------
        job->saved_tty_state_flag = true;
        termstate_give_terminal_back_to_shell();
        //when i enter ctrl-c the child recieves the signal, kills itself, then sends SIGCHLD 
        // when there are multiple co
    }
    switch(WTERMSIG(status))
    {
        case SIGFPE: printf("floating point exception\n");
        break;
        case SIGSEGV: printf("segmentation fault\n");
        break;
        case SIGABRT: printf("aborted\n");
        break;
        case SIGKILL: printf("killed\n");
        break;
        case SIGTERM: printf("Terminated\n");
        break;
    }
}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;) {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Check if stdin is a terminal before outputting a prompt */
        char *prompt = isatty(0) ? build_prompt() : NULL; // Generate prompt
        char *cmdline = readline(prompt); // Read user input
        free(prompt); // Free the prompt to avoid memory leaks

        /* Break loop if user inputs EOF (Ctrl+D) */
        if (cmdline == NULL) break;

        /* Parse the command line input */
        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        free(cmdline); // Free the input line after parsing
        /* Skip loop iteration on parsing error */
        if (cline == NULL) continue;

        /* Skip loop iteration if user just hit enter */
        if (list_empty(&cline->pipes)) {
            ast_command_line_free(cline);
            continue;
        }

        /* Process built-in commands (exit, kill, jobs, fg, bg, stop) */
        struct ast_pipeline *pipeline = list_entry(list_begin(&cline->pipes), struct ast_pipeline, elem);
        struct ast_command *command = list_entry(list_begin(&pipeline->commands), struct ast_command, elem);
        /* Exit command */
        if(strcmp(command->argv[0], "exit") == 0) {
            exit(0);
        }
        /* Kill command */
        if(strcmp(command->argv[0], "kill") == 0) {
            pid_t kill_pid = atoi(command->argv[1]);
            struct job *job = jid2job[kill_pid];
            if (killpg(job->gpid, SIGTERM) == -1) {
                perror("Error while sending SIGTERM");
            } else {
                printf("SIGTERM sent successfully\n");
            }
        }
        /* Jobs command */
        else if(strcmp(command->argv[0], "jobs") == 0) {
            struct list_elem *k;
            for (k = list_begin(&job_list); k != list_end(&job_list); k = list_next(k)) {
                struct job *jobs = list_entry(k, struct job, elem);
                if(jobs->status != TERMINATED) {
                    print_job(jobs);
                }
            }
        }
        
        /* Handle 'fg' command to bring a job to the foreground */
        else if(strcmp(command->argv[0], "fg") == 0)
        {
            pid_t fg_jid = atoi(command->argv[1]); // Set job status to foreground
            jid2job[fg_jid]->status = FOREGROUND;
            signal_block(SIGCHLD);
            if(killpg(jid2job[fg_jid]->gpid, SIGCONT)) 
            {
                perror("error sending cont signal");  // Error handling if SIGCONT fails
            }

            if(jid2job[fg_jid]->saved_tty_state_flag)
            {
                termstate_give_terminal_to(&jid2job[fg_jid]->saved_tty_state, jid2job[fg_jid]->gpid);
            }
            else
            {
                termstate_give_terminal_to(NULL, jid2job[fg_jid]->gpid);
                jid2job[fg_jid]->saved_tty_state_flag = true;
            }

            print_cmdline(jid2job[fg_jid]->pipe);
            printf("\n");
            
            wait_for_job(jid2job[fg_jid]);
            signal_unblock(SIGCHLD);
            termstate_give_terminal_back_to_shell();
        }
         /* Handle 'bg' command to run a job in the background */
        else if (strcmp(command->argv[0], "bg") == 0)
        {
            pid_t bg_jid = atoi(command->argv[1]); // Convert job ID from string to int
            jid2job[bg_jid]->status = BACKGROUND;
            killpg(jid2job[bg_jid]->gpid, SIGCONT); // Send SIGCONT signal to resume the job in the background
            termstate_give_terminal_back_to_shell();
        }
        /* Handle 'stop' command to stop a job */
        else if (strcmp(command->argv[0], "stop") == 0)
        {
            pid_t kill_pid = atoi(command->argv[1]);
            struct job *job = jid2job[kill_pid];
            job->status = STOPPED;

            if (killpg(job->gpid, SIGSTOP) == -1) {
                perror("Error while sending SIGTERM");
            } else {
                printf("SIGTERM sent successfully\n"); // Confirmation message for successful SIGTERM
            }
        }
        else
        
      {    
    // Iterate through each pipeline extracted from the command line
    struct list_elem *p, *c;
    for (p = list_begin(&cline->pipes); p != list_end(&cline->pipes); p = list_next(p)) {
        signal_block(SIGCHLD);
        // Extract the current pipeline from the list of pipelines
        struct ast_pipeline *pipeline = list_entry(p, struct ast_pipeline, elem);
        // Add a new job for the current pipeline
        struct job *job = add_job(pipeline);
        int pipe[list_size(&pipeline->commands)][2];
        for(int x = 0; x < list_size(&pipeline->commands); x++) {
            // Create a pipe for each command in the pipeline, set to close on exec
            pipe2(pipe[x], O_CLOEXEC);
        }                             
        // Keep track of the previous command in the pipeline to setup redirection
        struct list_elem *prev_c = NULL;
        int count = 0;
        
                //fprintf(stderr, "\nJOB ID: %d\n", job->jid);
                        //if (job->pipe == pipeline){
                            
                            //struct list_elem *prev_c = NULL;
                posix_spawn_file_actions_t file_actions;
                posix_spawnattr_t spawn_attr;
                for(c = list_begin(&pipeline->commands); c != list_end(&pipeline->commands); c = list_next(c))
                {
                    

                    // Retrieve the current command from the list
                    struct ast_command *command = list_entry(c, struct ast_command, elem);

                    pid_t pid;
                    // Print job ID and uninitialized PID for background jobs before spawning
                    if(job->status == BACKGROUND)
                    {
                        printf("[%d]      %d\n", job->jid, pid);
                    }
                    
                    posix_spawnattr_init(&spawn_attr);
                    posix_spawn_file_actions_init(&file_actions);
                    // Setup input redirection if specified
                    if(pipeline->iored_input)
                    {
                        int status = posix_spawn_file_actions_addopen(&file_actions, 0, pipeline->iored_input, O_RDONLY, 0666);
                        if (status != 0)
                        {
                            printf("posix_spawn_file_actions_addopen did not succeed");
                            break;
                        }
                    }
                    // Set up output redirection if specified
                    if(pipeline->iored_output)
                    {
                        if(pipeline->append_to_output)
                        {
                            int status = posix_spawn_file_actions_addopen(&file_actions, 1, pipeline->iored_output, O_WRONLY|O_APPEND|O_CREAT, 0666);
                            if (status != 0)
                            {
                                printf("posix_spawn_file_actions_addopen did not succeed");
                                break;
                            }
                        }
                        else
                        {
                            int status = posix_spawn_file_actions_addopen(&file_actions, 1, pipeline->iored_output, O_WRONLY|O_CREAT|O_TRUNC, 0666);
                            if (status != 0)
                            {
                                printf("posix_spawn_file_actions_addopen did not succeed");
                                break;
                            }
                        }
                    }
                    // Set process group related flags for background or foreground execution
                    if(pipeline->bg_job)
                    {
                        posix_spawnattr_setflags(&spawn_attr, POSIX_SPAWN_SETPGROUP);
                    }
                    else
                    {
                        posix_spawnattr_setflags(&spawn_attr, POSIX_SPAWN_TCSETPGROUP | POSIX_SPAWN_SETPGROUP);
                        posix_spawnattr_tcsetpgrp_np(&spawn_attr, termstate_get_tty_fd());
                    }
                    posix_spawnattr_setpgroup(&spawn_attr, 0);
                    
                    // Duplicate stderr to stdout if specified
                    if(command->dup_stderr_to_stdout)
                    {
                        int status = posix_spawn_file_actions_adddup2(&file_actions, 1, 2);
                        if(status != 0)
                        {
                            fprintf(stderr, "posix_spawn_file_actions_adddup2 has thrown an error %d\n", status);
                            break;
                        }
                    }
                    // Setup pipe redirection for commands not at the end of the pipeline
                    if (c != list_end(&pipeline->commands) && list_next(c) != list_end(&pipeline->commands)) {
                        posix_spawn_file_actions_adddup2(&file_actions, pipe[count][1], STDOUT_FILENO);
                        // Optionally duplicate stderr to the same pipe if required
                        if(command->dup_stderr_to_stdout)
                        {
                            int status = posix_spawn_file_actions_adddup2(&file_actions, pipe[count][1], 2);
                            if(status != 0)
                            {
                                fprintf(stderr, "posix_spawn_file_actions_adddup2 has thrown an error %d\n", status);
                                break;
                            }
                        }
                        //printf("Write end of pipe: %d\n", pipefd[1]);
                    }

                    // middle process makes 2 dup calls while the rest make 1
                    // close all pipes at the end of the foreloop

                                  
                if(prev_c != NULL){
                    posix_spawn_file_actions_adddup2(&file_actions, pipe[count - 1][0], STDIN_FILENO); // Redirect stdin from previous pipe
                    // Redirect stderr to stdout if specified by command
                    if(command->dup_stderr_to_stdout) {
                        int status = posix_spawn_file_actions_adddup2(&file_actions, pipe[count - 1][1], 2);
                        if(status != 0) {
                            // Error handling if dup2 fails
                            fprintf(stderr, "posix_spawn_file_actions_adddup2 has thrown an error %d\n", status);
                            break; // Exit loop on error
                        }
                    }
                }
                // Spawn a new process for the command
                int status = posix_spawnp(&pid, command->argv[0], &file_actions, &spawn_attr, command->argv, environ);
                
                // Check for errors in spawning the process
                if (status != 0) {
                    // Print error message if command cannot be found
                    printf("%s: No such file or directory\n", command->argv[0]);
                    termstate_give_terminal_back_to_shell(); // Return terminal control to shell
                    continue; // Skip further processing for this command
                } else {
                    // Set job status based on whether it's a background job
                    if(pipeline->bg_job) {
                        job->status = BACKGROUND; // Mark job as running in background
                        printf("[%u] %u\n", job->jid, pid); 
                        termstate_save(&job->saved_tty_state); // Save current terminal state
                        job->saved_tty_state_flag = true; 
                    } else {
                        job->status = FOREGROUND; 
                        termstate_give_terminal_to(NULL, pid); // Give terminal control to the process
                    }
                    // Update job information with new process ID
                    if(job->num_processes_alive == 0) {
                        job->gpid = pid; // Set group process ID for the job
                    }
                    job->num_processes_alive += 1; // Increment number of alive processes in the job
                    job->pids[job->num_processes_alive] = pid; // Record new process ID in job
                    job->saved_tty_state_flag = false; 
                }

                    //printf("PID of child %s:  %d\n", command->argv[0], job->gpid);
                    
                    /**if(prev_c != NULL){
                        close(pipefd[0]);
                        //printf("Closing file descriptor: %d\n", pipefd[0]);
                        
                    }
                    if (c != list_end(&pipeline->commands) && list_next(c) != list_end(&pipeline->commands)) {
                        close(pipefd[1]);
                    }
                    if(list_next(c) != list_end(&pipeline->commands))
                    {
                        close(pipefd[1]);
                        //printf("Closing file descriptor: %d\n", pipefd[1]);
                    }**/
                    prev_c = c;

                    count++;
                }
                // Close all pipe file descriptors after command execution
                for(int x = 0; x < list_size(&pipeline->commands); x++)
                {
                    close(pipe[x][0]); //Close the read end of the pipe
                    close(pipe[x][1]); //Close the write end of the pipe
                }  
                // Wait for job to finish if it's not running in the background
                if(job->status != BACKGROUND)
                {
                    wait_for_job(job);
                    if(job->status == FOREGROUND) //Wait for foreground process to complete
                    {
                        // If job is still in foreground after completion, consider deleting it
                        delete_job(job);
                    }
                }
                // Clean up any jobs that have no processes alive 
                struct list_elem *r;
                for(r = list_begin(&job_list); r != list_end(&job_list); r = list_next(r))
                {
                    struct job *dead_job = list_entry(r, struct job, elem);
                    if(dead_job->num_processes_alive == 0)
                    {
                        delete_job(dead_job); // Remove job from job list and free its resources
                    }
                }
                // close pipes after forloop
                signal_unblock(SIGCHLD);
                termstate_give_terminal_back_to_shell();
                
            }
        }
            
        
        /* Output a representation of
           the entered command line */

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */

        //ast_command_line_free(cline);
    }
    return 0;
}
