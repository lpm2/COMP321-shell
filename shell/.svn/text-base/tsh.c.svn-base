/* 
 * tsh - A tiny shell program with job control
 * 
 * <Put your name(s) and login ID(s) here>
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sig2str.h"

/* Constants - You may assume these are large enough. */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

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
 * At most 1 job can be in the FG state.
 */

extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct Job {                /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};
typedef struct Job *JobP;
struct Job jobs[MAXJOBS];   /* The job list */

/* Here are the prototypes for the functions that you will implement: */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void initpath(char *pathstr);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you: */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(JobP job);
void initjobs(JobP jobs);
int maxjid(JobP jobs); 
int addjob(JobP jobs, pid_t pid, int state, char *cmdline);
int deletejob(JobP jobs, pid_t pid); 
pid_t fgpid(JobP jobs);
JobP getjobpid(JobP jobs, pid_t pid);
JobP getjobjid(JobP jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(JobP jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int
main(int argc, char **argv) 
{
	int c;
	char *path = NULL;
	char cmdline[MAXLINE];
	int emit_prompt = 1;	/* Emit a prompt by default. */

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
			verbose = 1;
			break;
		case 'p':             /* Don't print a prompt. */
			/* This is handy for automatic testing. */
			emit_prompt = 0;
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

	/* Initialize the job list. */
	initjobs(jobs);

	/* Execute the shell's read/eval loop. */
	while (1) {

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
 */
void
eval(char *cmdline) 
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	cmdline = (char *)cmdline;
}

/* 
 * parseline - Parse the command line and build the argv array.
 *
 * Requires:
 *   cmdline is a NUL ('\0') terminated string with a trailing
 *   '\n' character.  cmdline must contain less than MAXARGS
 *   arguments.
 *
 * Effects:
 *   Builds argv array from space delimited arguments on the cmdline.
 *   The final element of argv is set to NULL.  Characters enclosed in
 *   single quotes are treated as a single argument.  Return true if
 *   the user has requested a BG job, false if the user has requested
 *   a FG job.
 */
int
parseline(const char *cmdline, char **argv) 
{
	static char array[MAXLINE]; /* holds local copy of command line */
	char *buf = array;          /* ptr that traverses command line */
	char *delim;                /* points to first space delimiter */
	int argc;                   /* number of args */
	int bg;                     /* background job? */

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
 */
int
builtin_cmd(char **argv) 
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	argv = (char **)argv;
	return (0);     /* This is not a builtin command. */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands.
 */
void
do_bgfg(char **argv) 
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	argv = (char **)argv;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process.
 */
void
waitfg(pid_t pid)
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	pid = (pid_t)pid;
}

/* 
 * initpath - Perform all necessary initialization of the search path,
 *   which may be simply saving the path.
 *
 * Requires:
 *   pathstr is the valid path from the environment.
 *
 * Effects:
 *   <Fill In>
 */
void
initpath(char *pathstr)
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	pathstr = (char *)pathstr;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal.  The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void
sigchld_handler(int sig) 
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	sig = (int)sig;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void
sigint_handler(int sig) 
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	sig = (int)sig;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard.  Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void
sigtstp_handler(int sig) 
{

	/* Prevent an "unused parameter" warning.  REMOVE THIS STATEMENT! */
	sig = (int)sig;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/*
 * clearjob
 *
 * Requires:
 *   job points to a valid job struct.
 *
 * Effects:
 *   Clear the entries in a job struct. 
 */
void
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
 *   jobs points to an array of MAXJOBS job structs.
 *
 * Effects:
 *   Initialize the job list. 
 */
void
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
 *   Nothing
 *
 * Effects:
 *   Returns the largest allocated job ID. 
 */
int
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
 *   jobs points to an array of MAXJOBS job structs and
 *   cmdline is a properly terminated string.
 *
 * Effects: 
 *   Add a job to the job list. 
 */
int
addjob(JobP jobs, pid_t pid, int state, char *cmdline) 
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
 *   jobs points to an array of MAXJOBS job structs.
 *
 * Effects:
 *   Delete a job whose PID=pid from the job list. 
 */
int
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
 *   jobs points to an array of MAXJOBS job structs.
 *
 * Effects:
 *   Return PID of current foreground job, 0 if no such job. 
 */
pid_t
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
 *   jobs points to an array of MAXJOBS job structs.
 *
 * Effects:
 *   Return a pointer to the job struct with process ID pid, or
 *   NULL if no such job.
 */
JobP
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
 *   jobs points to an array of MAXJOBS job structs.
 *
 * Effects:
 *   Return a pointer to the job struct with job ID jid, or
 *   NULL if no such job.
 */
JobP
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
 *   Nothing
 *
 * Effects:
 *   Return the job ID for the job with process ID pid,
 *   or 0 if no such process.
 */
int
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
 *   jobs points to an array of MAXJOBS job structs.
 *
 * Effects:
 *   Prints the job list.
 */
void
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

/******************************
 * end job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage
 *
 * Requires:
 *   Nothing
 *
 * Effects:
 *   Print a help message.
 */
void
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
 *   msg is a properly terminated string.
 *
 * Effects:
 *   Prints a unix-style error message and terminates the program.
 */
void
unix_error(char *msg)
{

	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * app_error
 *
 * Requires:
 *   msg is a properly terminated string.
 *
 * Effects:
 * Prints msg and terminates the program.
 */
void
app_error(char *msg)
{

	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Signal
 *
 * Requires:
 *   Nothing
 *
 * Effects:
 *  Wrapper for the sigaction function to register a signal handler.
 *  Behaves similarly to "sigset" but restarts system calls, if possible.
 */
handler_t *
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
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 *
 * Requires:
 *   Nothing
 *
 * Effects:
 *   Terminates the program.
 */
void
sigquit_handler(int sig) 
{

	assert(sig == SIGQUIT);
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
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
