/* 
 * tsh - A tiny shell program with job control
 * 
 * Xin Huang xyh1
 * Leo Meister lpm2
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sig2str.h"

/* Constants - You may assume these are large enough. */
#define MAXLINE      1024   /* max line size */
#define MAXARGS       128   /* max args on a command line */
#define MAXJOBS        16   /* max jobs at any point in time */
#define MAXJID   (1 << 16)  /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most one job can be in the FG state.
 */

struct Job {                /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};
typedef struct Job *JobP;
struct Job jobs[MAXJOBS];   /* The jobs list */

int nextjid = 1;            /* next job ID to allocate */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
bool verbose = false;       /* if true, print additional output */

/* You will implement the following functions: */

static void eval(const char *cmdline);
static int builtin_cmd(char **argv);
static void do_bgfg(char **argv);
static void waitfg(pid_t pid);
static void initpath(const char *pathstr);

static void sigchld_handler(int signum);
static void sigint_handler(int signum);
static void sigtstp_handler(int signum);

/* We've provided the following functions to you: */

static int parseline(const char *cmdline, char **argv); 

static void sigquit_handler(int signum);

static void clearjob(JobP job);
static void initjobs(JobP jobs);
static int maxjid(JobP jobs); 
static int addjob(JobP jobs, pid_t pid, int state, const char *cmdline);
static int deletejob(JobP jobs, pid_t pid); 
static pid_t fgpid(JobP jobs);
static JobP getjobpid(JobP jobs, pid_t pid);
static JobP getjobjid(JobP jobs, int jid); 
static int pid2jid(pid_t pid); 
static void listjobs(JobP jobs);

static void usage(void);
static void unix_error(const char *msg);
static void app_error(const char *msg);

typedef void handler_t(int);
static handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 *
 * <???>
 */
int
main(int argc, char **argv) 
{
	int c;
	char cmdline[MAXLINE];
	char *path = NULL;
	bool emit_prompt = true;	/* Emit a prompt by default. */

	/*
	 * Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout).
	 */
	dup2(1, 2);

	/* Parse the command line. */
	while ((c = getopt(argc, argv, "hvp")) != -1) {
		switch (c) {
		case 'h':             /* Print a help message. */
			usage();
			break;
		case 'v':             /* Emit additional diagnostic info. */
			verbose = true;
			break;
		case 'p':             /* Don't print a prompt. */
			/* This is handy for automatic testing. */
			emit_prompt = false;
			break;
		default:
			usage();
		}
	}

	/* Install the signal handlers. */

	/* These are the ones you will need to implement: */
	Signal(SIGINT,  sigint_handler);   /* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

	/* This one provides a clean way to kill the shell. */
	Signal(SIGQUIT, sigquit_handler); 

	/* Initialize the search path. */
	path = getenv("PATH");
	initpath(path);

	/* Initialize the jobs list. */
	initjobs(jobs);

	/* Execute the shell's read/eval loop. */
	while (true) {

		/* Read the command line. */
		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}
		if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
			app_error("fgets error");
		if (feof(stdin)) { /* End of file (ctrl-d) */
			fflush(stdout);
			exit(0);
		}

		/* Evaluate the command line. */
		eval(cmdline);
		fflush(stdout);
		fflush(stdout);
	}

	exit(0); /* Control never reaches here. */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in.
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately.  Otherwise, fork a child process and
 * run the job in the context of the child.  If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 *
 * Requires:
 *  A string representing a command for the shell to execute
 *
 * Effects:
 *  Executes the given command, either as a built-in command or as an executable  	
 * file depending on the arguments
 */
static void
eval(const char *cmdline) 
{

	int bg_job;	/* whether the job is to run in the background */
	int pid;	/* the process id returned from fork */
	/* string array to store command line arguments */
	char **argv = malloc(sizeof(char **));	
	
	bg_job = parseline(cmdline, argv);
	
	/* If nothing is entered, don't evaluate */
	if (argv[0] == NULL)
		return;
	/* Run the command if it is builtin, otherwise execute
	 * the executable specified by the first argument
	 */
	else if (!builtin_cmd(argv)) {
		
		/* Block sigchld signals in the parent */
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGCHLD);
		sigprocmask(SIG_BLOCK, &mask, NULL);
					
		/* fork a child process to run the job, setting its groupd id,
		 * unblocking the sig_child signal, and using execvp to
		 * search the search path if necessary
		 */
		if ((pid = fork()) == 0) {
			setpgid(0, 0);
			if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
				unix_error("Problem unblocking SIGCHLD!");			
			if (execvp(argv[0], argv) == -1) {
				printf("%s: Command not found\n", argv[0]);
				exit(0);
			}
		}

		/* In the parent process (the child terminates after the
		 * execvp call), add the job to the background or foreground
		 * as appropriate and then unblocking the child signal.
		 */
		if (bg_job) {
			if (!addjob(jobs, pid, BG, cmdline)) {
				if (verbose)
					printf("Error: Problem adding"
					    " background job!\n");
				exit(1);
			}
			if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
				unix_error("Problem unblocking SIGCHLD!");
			printf("[%d] (%d) %s", getjobpid(jobs, pid)->jid, pid, 
			    cmdline);
		} // end if
		else {
			if (!addjob(jobs, pid, FG, cmdline)) {
				if (verbose)
					printf("Error: Problem adding"
					    " foreground job!\n");
				exit(1);
			}
			if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
				unix_error("Problem unblocking SIGCHLD!");
			waitfg(pid);
		} // end else		
	} // end else if not built in

	return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 *
 * Requires:
 *  "cmdline" is a NUL ('\0') terminated string with a trailing
 *  '\n' character.  "cmdline" must contain less than MAXARGS
 *  arguments.
 *
 * Effects:
 *  Builds "argv" array from space delimited arguments on the command line.
 *  The final element of "argv" is set to NULL.  Characters enclosed in
 *  single quotes are treated as a single argument.  Returns true if
 *  the user has requested a BG job and false if the user has requested
 *  a FG job.
 */
static int
parseline(const char *cmdline, char **argv) 
{
	int argc;                   /* number of args */
	int bg;                     /* background job? */
	static char array[MAXLINE]; /* holds local copy of command line */
	char *buf = array;          /* ptr that traverses command line */
	char *delim;                /* points to first space delimiter */

	strcpy(buf, cmdline);
	buf[strlen(buf) - 1] = ' '; /* Replace trailing '\n' with space. */
	while (*buf && (*buf == ' ')) /* Ignore leading spaces. */
		buf++;

	/* Build the argv list */
	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	} else
		delim = strchr(buf, ' ');

	while (delim) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf && (*buf == ' ')) /* Ignore spaces. */
			buf++;

		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		} else
			delim = strchr(buf, ' ');
	}
	argv[argc] = NULL;
    
	if (argc == 0)  /* Ignore blank line. */
		return (1);

	/* Should the job run in the background? */
	if ((bg = (*argv[argc - 1] == '&')) != 0)
		argv[--argc] = NULL;

	return (bg);
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 *
 * Requires:
 * 	Command argument
 *
 * Effects:
 *	Checks if command argument is built in
 *	Runs built in command arguments
 *	The bg <job> command restarts <job> by sending it a SIGCONT signal, 
 * 		then runs it in the background.
 *	The fg <job> command restarts <job> by sending it a SIGCONT signal, 
 *		then runs it in the foreground. 
 * 	The <job> argument can be either a PID or a JID.
 */
static int
builtin_cmd(char **argv) 
{	
	/* Exit on quit */
	if (strcmp(argv[0], "quit") == 0)
		exit(0);
	/* Executes bg command */
	else if (strcmp(argv[0], "bg") == 0) {
		do_bgfg(argv);
		return(1);
	}
	/* Executes fg command */
	else if (strcmp(argv[0], "fg") == 0) {
		do_bgfg(argv);
		return(1);
	}
	/* Prints a list of all jobs */
	else if (strcmp(argv[0], "jobs") == 0) {
		listjobs(jobs);
		return (1);
	}
	else {
		if (verbose)
			printf("Error: No built in command, %s, found!", 
				argv[0]);
		return(0);
	}
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands.
 *
 * Requires:
 * 	Command argument
 *
 * Effects:
 *	Checks if command arguments fg and bg are built in
 *	Runs built in command arguments fg and bg
 */
static void
do_bgfg(char **argv) 
{
	if (strcmp(argv[0],"bg") != 0 && strcmp(argv[0],"fg") != 0) {
		if (verbose)
			printf("Error: Argument passed is neither bg nor fg!\n");
		return;
	}

	/* Cannot execute if no id is specified */
	if (argv[1] == NULL) {
		printf("%s command requires PID or %%jobid argument\n", 
			argv[0]);
		return;
	}
	
	JobP bgfgJob; 	/* pointer to the job with the given id */
	int pid;	/* process id argument*/
	int jid;	/* job id argument */
	char pj_id_flag = 3; /* flag for determining proper error message */

	/* Gets the job of the corresponding process id */
	if (argv[1][0] != '%') {
		pid = atoi(argv[1]);
		bgfgJob = getjobpid(jobs, pid);
		if (isdigit(argv[1][0]))
			pj_id_flag = 0;
	}
	/* Gets the job of the corresponding job id */
	else {
		char *ret;
		const char ch = '%';
   		ret = strchr(argv[1], ch);
		jid = atoi(ret+1);
		bgfgJob = getjobjid(jobs, jid);
		pj_id_flag = 1;
	}

	/* Corner cases: No process or job given or an invalid argument */
	if (bgfgJob == NULL) {
		if (pj_id_flag == 0)
			printf("(%d): No such process\n", pid);
		else if (pj_id_flag == 1)
			printf("%s: No such job\n", argv[1]);
		else
			printf("%s: argument must be a PID or %%jobid\n", 
				argv[0]);
		return;
	}

	/* Executes bg by continuing the job in the background */
	if (strcmp(argv[0], "bg") == 0) {
		bgfgJob->state = BG;
		printf("[%d] (%d) %s", pid2jid(bgfgJob->pid), 
		    bgfgJob->pid, bgfgJob->cmdline);
		kill(-bgfgJob->pid, SIGCONT);
	}
	/* Executes fg by continuing the job in the foreground */
	else {
		bgfgJob->state = FG;
		kill(-bgfgJob->pid, SIGCONT);
		waitfg(bgfgJob->pid);
	}
}

/* 
 * waitfg - Block until process pid is no longer the foreground process.
 * Requires: 
 * 	Process id
 *
 * Effects: 
 * 	sleeps until the foreground job is no longer active
 */
static void
waitfg(pid_t pid)
{

	/* Sleep while the given process is still active in the foreground */
	while (fgpid(jobs) == pid) {
		if (verbose)
			printf("Sleeping...\n");
		sleep(1);
	}
}

/* 
 * initpath - Perform all necessary initialization of the search path,
 *   which may be simply saving the path.
 *
 * Requires:
 *   pathstr is the valid path from the environment.
 *
 * Effects:
 *   If verbose output is selected, prints the path, otherwise does nothing
 *   No preprocessing required because of the use of execvp
 */
static void
initpath(char *pathstr)
{	
	if (verbose) {
		if (pathstr == NULL) 
			printf("Warning: Path is NULL!\n");
		else
			printf("Path= %s\n", pathstr);		
	} 	
	
	return;
}

/*
 * The signal handlers follow.
 */

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *  a child job terminates (becomes a zombie), or stops because it
 *  received a SIGSTOP or SIGTSTP signal.  The handler reaps all
 *  available zombie children, but doesn't wait for any other
 *  currently running children to terminate.  
 *
 * Requires:
 *  A signal number
 *
 * Effects:
 *  Reaps terminated children, changes the state of stopped children in the job 
 *  list to ST, removes terminated children from the jobs list, prints messages 
 *  if children received TSTP or INT signals
 */
static void
sigchld_handler(int signum)
{
	pid_t pid;	/* the process id of the foreground process */
	int status;	/* the status of waitpid */

	/* make sure the given signal is a SIGCHLD signal */
	if (sig == SIGCHLD) {
		
		/* Handle reaping of all terminated child and handle
		 * stopped children
		 */
		while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
		
			JobP fgJob = getjobpid(jobs, pid);	
	
			if (verbose)
				printf("Handler handling child %d\n", (int)pid);
			
			/* If stopped, move the process to the background,
			 * and print the process if it was stopped or
			 * terminated due to a signal. Remove the
			 * job from the list if it was terminated.
			 */
			if (WIFSTOPPED(status) && fgJob != NULL) {
				
				fgJob->state = ST;
				printf("Job [%d] (%d) stopped by signal "
				    "SIGTSTP\n", 
				    pid2jid(fgJob->pid), fgJob->pid);
				    
			} else if (WIFSIGNALED(status) && fgJob != NULL) {
				
				printf("Job [%d] (%d) terminated by signal " 	
				    "SIGINT\n", 
				    pid2jid(fgJob->pid), fgJob->pid);
				deletejob(jobs, pid);
				
			} else if (WIFEXITED(status) && fgJob != NULL)
				deletejob(jobs, pid);
		}
	}

	return;assert(signum == SIGCHLD);
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *  user types ctrl-c at the keyboard.  Catch it and send it along
 *  to the foreground job.  
 *
 * Requires:
 *  Signal number
 *
 * Effects:
 *  Catches an interrupt signal and sends it to the foreground job
 */
static void
sigint_handler(int sig) 
{	
	if (sig != SIGINT) {
		if (verbose)
			printf("Error: SIGINT not received!\n");
	}
	/* Ensure that there is a job running in the foreground and forward
	 * the interrupt signal to it, otherwise don't do anything
	 */
	else {
		pid_t fg_pid = fgpid(jobs);
		if (!fg_pid) {
			if (verbose)
				printf("Error: No such job to STOP!\n");
			return;
		}

		JobP fgJob = getjobpid(jobs, fg_pid);
		if (fgJob == NULL || kill(-fgJob->pid, sig) == -1)
			unix_error("Unable to forward SIGINT!\n");
	}
	
	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *  the user types ctrl-z at the keyboard.  Catch it and suspend the
 *  foreground job by sending it a SIGTSTP.  
 *
 * Requires:
 *  Signal number
 *
 * Effects:
 *  Catches a TSTP signal and sends it to the foreground job
 */
static void
sigtstp_handler(int sig) 
{
	if (sig != SIGTSTP) {
		if (verbose)
			printf("Error: SIGTSTP not received!\n");
	}
	/* Ensure that there is a job running in the foreground and 
	 * forward the tstp signal to it, otherwise don't do anything
	 */
	else {
		pid_t fg_pid = fgpid(jobs);
		if (!fg_pid) {
			if (verbose)
				printf("Error: No such job to STOP!\n");
			return;
		}

		JobP fgJob = getjobpid(jobs, fg_pid);	
		if (fgJob == NULL || kill(-fgJob->pid, sig) == -1)
			unix_error("Unable to forward SIGTSTP!\n"); 
	}
	return;
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *  child shell by sending it a SIGQUIT signal.
 *
 * Requires:
 *  "signum" is SIGQUIT.
 *
 * Effects:
 *  Terminates the program.
 */
static void
sigquit_handler(int signum)
{

	assert(signum == SIGQUIT);
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}

/*
 * This comment marks the end of the signal handlers.
 */

/*
 * The following helper routines manipulate the jobs list.
 */

/*
 * clearjob
 *
 * Requires:
 *  "job" points to a job structure.
 *
 * Effects:
 *  Clears the fields in the referenced job structure.
 */
static void
clearjob(JobP job)
{

	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/*
 * initjobs
 * 
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *  Initializes the jobs list to an empty state.
 */
static void
initjobs(JobP jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/*
 * maxjid
 *
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *  Returns the largest allocated job ID.
 */
static int
maxjid(JobP jobs) 
{
	int i, max = 0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return (max);
}

/*
 * addjob
 *
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures, and "cmdline" is
 *  a properly terminated string.
 *
 * Effects: 
 *  Adds a job to the jobs list. 
 */
static int
addjob(JobP jobs, pid_t pid, int state, const char *cmdline)
{
	int i;
    
	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(jobs[i].cmdline, cmdline);
			if (verbose) {
				printf("Added job [%d] %d %s\n", jobs[i].jid,
				    (int)jobs[i].pid, jobs[i].cmdline);
			}
			return (1);
		}
	}
	printf("Tried to create too many jobs\n");
	return (0);
}

/*
 * deletejob 
 * 
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *  Deletes a job from the jobs list whose PID equals "pid".
 */
static int
deletejob(JobP jobs, pid_t pid) 
{
	int i;

	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs) + 1;
			return (1);
		}
	}
	return (0);
}

/*
 * fgpid
 * 
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *  Returns the PID of the current foreground job or 0 if no foreground
 *  job exists.
 */
static pid_t
fgpid(JobP jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return (jobs[i].pid);
	return (0);
}

/*
 * getjobpid
 * 
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *  Returns a pointer to the job structure with process ID "pid" or NULL if
 *  no such job exists.
 */
static JobP
getjobpid(JobP jobs, pid_t pid)
{
	int i;

	if (pid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * getjobjid 
 * 
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *  Returns a pointer to the job structure with job ID "jid" or NULL if no
 *  such job exists.
 */
static JobP
getjobjid(JobP jobs, int jid) 
{
	int i;

	if (jid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * pid2jid 
 * 
 * Requires:
 *  Nothing.
 *
 * Effects:
 *  Returns the job ID for the job with process ID "pid" or 0 if no such
 *  job exists.
 */
static int
pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (jobs[i].jid);
	return (0);
}

/*
 * listjobs
 *
 * Requires:
 *  "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *  Prints the jobs list.
 */
static void
listjobs(JobP jobs) 
{
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, (int)jobs[i].pid);
			switch (jobs[i].state) {
			case BG: 
				printf("Running ");
				break;
			case FG: 
				printf("Foreground ");
				break;
			case ST: 
				printf("Stopped ");
				break;
			default:
				printf("listjobs: Internal error: "
				    "job[%d].state=%d ", i, jobs[i].state);
			}
			printf("%s", jobs[i].cmdline);
		}
	}
}

/*
 * This comment marks the end of the jobs list helper routines.
 */

/*
 * Other helper routines follow.
 */

/*
 * usage
 *
 * Requires:
 *  Nothing.
 *
 * Effects:
 *  Prints a help message.
 */
static void
usage(void) 
{

	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}

/*
 * unix_error
 *
 * Requires:
 *  "msg" is a properly terminated string.
 *
 * Effects:
 *  Prints a Unix-style error message and terminates the program.
 */
static void
unix_error(const char *msg)
{

	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * app_error
 *
 * Requires:
 *  "msg" is a properly terminated string.
 *
 * Effects:
 *  Prints "msg" and terminates the program.
 */
static void
app_error(const char *msg)
{

	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Signal
 *
 * Requires:
 *  "signum" is a valid signal number, and "handler" is a valid signal
 *  handler.
 *
 * Effects:
 *  Registers a new handler for the specified signal, returning the old
 *  handler.  Behaves similarly to "sigset" but restarts system calls
 *  when possible.
 */
static handler_t *
Signal(int signum, handler_t *handler) 
{
	struct sigaction action, old_action;

	action.sa_handler = handler;  
	sigemptyset(&action.sa_mask); /* Block sigs of type being handled. */
	action.sa_flags = SA_RESTART; /* Restart syscalls if possible. */
	if (sigaction(signum, &action, &old_action) < 0)
		unix_error("Signal error");
	return (old_action.sa_handler);
}

/*
 * The last lines of this file configure the behavior of the "Tab" key in
 * emacs.  Emacs has a rudimentary understanding of C syntax and style.  In
 * particular, depressing the "Tab" key once at the start of a new line will
 * insert as many tabs and/or spaces as are needed for proper indentation.
 */

/* Local Variables: */
/* mode: c */
/* c-default-style: "bsd" */
/* c-basic-offset: 8 */
/* c-continued-statement-offset: 4 */
/* indent-tabs-mode: t */
/* End: */