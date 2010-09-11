/*
 * Copyright (C) 1998 - 2007 Bob Beck (beck@openbsd.org).
 *
 * Copyright (C) 2010 Andreas Bertheussen (andreas@elektronisk.org).
 *	- based on $OpenBSD: authpf.c,v 1.115 2010/09/02 14:01:04 sobrado Exp$	
 *	- adapted for linux/ipset
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <err.h>
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>

#include <arpa/inet.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <pwd.h>

#include <sys/param.h>

#include <signal.h>

#include "pathnames.h"

size_t cplen = 0;
char pidfile[MAXPATHLEN];

FILE	*pidfp;
int	pidfd = -1;

char ipsrc[128]; /* xxx.xxx.xxx.xxx\0 */ 
char luser[32];
char proctitle[128];

static void	print_message(char *);
static int	user_allowed(char *);
static int	user_banned(char *);
static int	change_filter(int, const char *, const char *);
static int	change_table(int, const char *);
static void	do_death(int);
static void	need_death(int signo); /* signal handler */
volatile sig_atomic_t	want_death;

int main(int argc, char *argv[]) {
	int lockcnt = 0,n = 0;
	char *cp, *shell;
	struct in6_addr	 ina;
	uid_t uid;
	gid_t gid;
	struct passwd *pw;
	openlog("authipd",LOG_PID|LOG_NDELAY,LOG_DAEMON);
	
	if ((cp = getenv("SSH_TTY")) == NULL) {
		syslog(LOG_ERR, "non-interactive session connection for authipt");
		exit(1);
	}
	
	if ((cp = getenv("SSH_CLIENT")) == NULL) {
		syslog(LOG_ERR, "could not determine connection source");
		exit(1);
	}
	strncpy(ipsrc, cp, sizeof(ipsrc)); /* fit the stirng into ipsrc */
	if (strlen(ipsrc) < strlen(cp)){
		syslog(LOG_ERR, "SSH_CLIENT variable was too long");
		exit(1);
	}

	cp = strchr(ipsrc, ' '); /* Look for the space delimiter after IP address */
	if (cp == NULL) {
		syslog(LOG_ERR, "SSH_CLIENT variable was corrupt: %s", ipsrc);
		exit(1);
	}
	*cp = '\0';
	
	/* IPv6, not supported by ipset :/ */
	if (inet_pton(AF_INET, ipsrc, &ina) != 1 /*&&
		inet_pton(AF_INET6, ipsrc, &ina) != 1*/) {
		syslog(LOG_ERR, "could not determine IP from SSH_CLIENT %s", ipsrc);
		exit(1);
	}
	uid = getuid();
	pw = getpwuid(uid);
	if (pw == NULL) {
		syslog(LOG_ERR, "could not find user for uid %u", uid);
		exit(1);	
	}
	shell = pw->pw_shell; 
	
	/* Make sure the users shell is set to authipf (user is allowed to run authipf) */
	if (strcmp(shell, PATH_AUTHIPT_SHELL)) {
		syslog(LOG_ERR, "wrong shell for user %s, uid %u", pw->pw_name, pw->pw_uid);
		exit(1);
	}
	
	strncpy(luser, pw->pw_name, sizeof(luser));
	if (strlen(pw->pw_name) > strlen(luser)) {
		syslog(LOG_ERR, "username was too long: %s", pw->pw_name);
		exit(1);
	}

	/* The filename to the file for the users IP, e.g. /var/authipt/192.168.2.44 */
	n = snprintf(pidfile, sizeof(pidfile),"%s/%s",
		PATH_PIDFILE,
		ipsrc
		);

	/* a return value of (sizeof(pidfile)) or more means output was truncated */
	if (n < 0 || (u_int)n >= sizeof(pidfile)) {
		syslog(LOG_ERR, "path to pidfile was too long");
		exit(1);
	}
	
	signal(SIGTERM, need_death);
	signal(SIGINT, need_death);
	signal(SIGALRM, need_death);
	signal(SIGPIPE, need_death);
	signal(SIGHUP, need_death);
	signal(SIGQUIT, need_death);
	signal(SIGTSTP, need_death);

	/*
	 * If someone else is already using this ip, then this person
	 * wants to switch users - so kill the old process and exit
	 * as well.
	 *
	 * Note, we could print a message and tell them to log out, but the
	 * usual case of this is that someone has left themselves logged in,
	 * with the authenticated connection iconized and someone else walks
	 * up to use and automatically logs in before using. If this just
	 * gets rid of the old one silently, the new user never knows they
	 * could have used someone else's old authentication. If we
	 * tell them to log out before switching users it is an invitation
	 * for abuse.
	 */
	
	do {
		int save_errno, otherpid = -1;
		char otherluser[32];
		
		if ((pidfd = open(pidfile, O_RDWR|O_CREAT, 0664)) == -1 ||
		    (pidfp = fdopen(pidfd, "r+")) == NULL) {
			if (pidfd != -1)
				close(pidfd);
			syslog(LOG_ERR, "cannot open or create %s: %s", pidfile, strerror(errno));
			goto die;
		}

		fchmod(pidfd, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);

		/* Try to get an exclusive lock on the pidfile. If successful, break */
		if (flock(fileno(pidfp), LOCK_EX|LOCK_NB) == 0)
			break;

		/* File is locked, read it to find and kill owner process */
		save_errno = errno;
		rewind(pidfp);
		if (fscanf(pidfp, "%d\n%31s\n", &otherpid, otherluser) != 2)
			otherpid = -1;
		syslog(LOG_DEBUG, "tried to lock %s, in use by pid %d: %s",
			pidfile, otherpid, strerror(save_errno));

		if (otherpid > 0) {
			syslog(LOG_INFO, "Killing existing auth for %s@%s (pid %d)",
				otherluser, ipsrc, otherpid);
			if (kill((pid_t) otherpid, SIGTERM) == -1) {
				syslog(LOG_INFO, "could not kill process %d: (%m)", otherpid);
			}
		}
		/*
		 * We try to kill the previous process and acquire the lock
 		 * for 10 seconds, trying once a second. if we can't after
 		 * 10 attempts we log an error and give up.
 		 */
 		if (want_death || ++lockcnt > 10) {
			if (!want_death)
				syslog(LOG_ERR, "could not kill previous authipt (pid %d) for IP %s", otherpid, ipsrc);
			fclose(pidfp);
			pidfp = NULL;
			pidfd = -1;
			goto dogdeath;
		}
		sleep(1);
		/* re-open, and try again. The previous authipt process
		 * we killed above should unlink the file and release
 		 * it's lock, giving us a chance to get it now
		 */
		fclose(pidfp);
		pidfp = NULL;
		pidfd = -1;
	} while (1);

	/* whack the group list */
	gid = getegid();
	if (setgroups(1, &gid) == -1) {
		syslog(LOG_INFO, "setgroups: %s", strerror(errno));
		do_death(0);
	}

	/* check if user is banned, TODO: implement explicit allowance */
	if (user_banned(luser)) {	
		syslog(LOG_INFO, "User %s was not allowed to authenticate", luser);
		sleep(10);
		do_death(0);
	}

	/* TODO: CONFIG FILE */

	/* TODO: remove stale rulesets */

	rewind(pidfp);
	fprintf(pidfp, "%ld\n%s\n", (long)getpid(), luser);
	fflush(pidfp);
	(void) ftruncate(fileno(pidfp),ftello(pidfp));

	if (change_filter(1,luser,ipsrc) == -1) {
		printf("Unable to modify filters\n");
		do_death(0);
	}
	if (change_table(1, ipsrc) == -1){
		printf("Unable to modify ip set\n");
		change_filter(0,luser,ipsrc);
		do_death(0);
	}

	/* Greet authenticated user */
	while (1) {
		syslog(LOG_INFO, "User %s@%s authenticated.", luser, ipsrc);
		struct stat sb;
		char *motdpath;
		FILE *f;
		printf("Hello %s - you are authenticated from host %s.\n", luser, ipsrc);
		snprintf(proctitle, sizeof(proctitle), "%s@%s", luser, ipsrc);
		prctl(PR_SET_NAME, proctitle, NULL, NULL, NULL);
		/* TODO: rename the process better than this - prctl has a limit of 15 letters */	

		if (asprintf(&motdpath, "%s/%s/motd",PATH_USER_DIR, luser) == -1)
			do_death(1);
		if (stat(motdpath, &sb) == -1 || ! S_ISREG(sb.st_mode)) {
			free(motdpath);
			motdpath = strdup(PATH_MOTD);
			if (motdpath == NULL)
				do_death(1);
			/* We don't care if the file in PATH_MOTD does not exist */
		}
		print_message(motdpath);
		free(motdpath);
		
		while(1) {
			sleep(10);
			if (want_death)
				do_death(1);
		}
	}

	return 0;
dogdeath:
	printf("\n\nAuthentication is unavailable due to technical difficulties.\n");
	print_message(PATH_PROBLEM);
	printf("Your authentication process (pid %ld) was unable to run\n", (long)getpid());
	sleep(180);
die:
	do_death(0);
	return 0;
}

static int user_allowed(char *name) {
	return 1; /* All allowed by default */
}

static int user_banned(char *name) {
	FILE	*f;
	int	 n;
	char	 tmp[MAXPATHLEN];
	n = snprintf(tmp, sizeof(tmp), "%s/%s/banned", PATH_USER_DIR, luser);
	if (n < 0 || (u_int)n >= sizeof(tmp)) {
		syslog(LOG_ERR, "banned file directory name for user %s, was too long", name);
		return(1); /* do not allow login */
	}
	if ((f = fopen(tmp, "r")) == NULL) {
		if (errno == ENOENT) {
			return(0); /* file does not exist - user is not banned */
		} else {
			syslog(LOG_ERR, "could not open banned file for user %s: %s",
				name,strerror(errno));
			return(1); /* do not allow login */
		}
	} else {
		/* file exists and user is explicitly banned */
		syslog(LOG_INFO, "User %s is banned - banfile exists", name);
		printf("Your account is banned from authentication.\n");
		print_message(tmp); /* print the contents of the "banned" file */
		fclose(f);
		return(1);
	}
}

/*
 * splatter a file to stdout - max line length of 1024,
 * used for spitting message files at users to tell them
 * they've been bad or we're unavailable.
 */
static void print_message(char *filename) {
	char	 buf[1024];
	FILE	*f;
	if ((f = fopen(filename, "r")) == NULL)
		return; /* fail silently, we don't care if it isn't there */
	do {
		if (fgets(buf, sizeof(buf), f) == NULL) {
			fflush(stdout);
			fclose(f);
			return;
		}
	} while (fputs(buf, stdout) != EOF && !feof(f));
	fflush(stdout);
	fclose(f);
}

/* Filter change is done externally through a shell script called modfilter. */
static int change_filter(int add, const char *luser, const char *ipsrc) {
	pid_t pid;
	gid_t gid;
	int s;
	char *pargv[7] = {PATH_MODFILTER, (char*)luser, "up", (char*)ipsrc, "0", PATH_USER_DIR, NULL};
	char pidstring[32] = "";

	snprintf(pidstring, 31, "%ld", (long)getpid());
	
	if (luser == NULL || !luser[0] || ipsrc == NULL || !ipsrc[0] || !pidstring[0]) {
		syslog(LOG_ERR, "invalid luser/ipsrc/pid");
		goto error;
	}
	pargv[4] = pidstring;
	if (!add) {
		syslog(LOG_INFO, "Removing potential rules for %s@%s, pid %s", luser, ipsrc, pidstring);
		pargv[2] = "down";
	} else {
		syslog(LOG_INFO, "Adding potential rules for %s@%s, pid %s", luser, ipsrc, pidstring);
	}

	switch(pid = fork()) {
		case -1: syslog(LOG_ERR, "fork for filter modification failed");
			goto error;
		case 0: gid = getgid();
			if (setregid(gid,gid) == -1) {
				syslog(LOG_ERR, "setregid: %s", strerror(errno));
			}
			execvp(PATH_MODFILTER, pargv);
			syslog(LOG_ERR, "exec of %s failed", PATH_MODFILTER);
			_exit(1);
		default: break;
	}
	waitpid(pid, &s, 0);
	
	return(0);
error:
	return(-1);
}

static int change_table(int add, const char *ipsrc) {
	/* make sure the table exists */
	pid_t pid;
	gid_t gid;
	int s;
	char *ipstr = NULL;
	char *pargv[5] = {PATH_IPSET, "-N", "authipt", "iphash", NULL};
	
	
	if (luser == NULL || !luser[0] || ipsrc == NULL || !ipsrc[0]) {
		syslog(LOG_ERR, "invalid luser/ipsrc");
		goto error;
	}
	
	switch(pid = fork()) {
		case -1: syslog(LOG_ERR, "fork for set creation failed");
			goto error;
		case 0: gid = getgid();
			if (setregid(gid,gid) == -1) {
				syslog(LOG_ERR, "setregid: %s", strerror(errno));
			}
			execvp(PATH_IPSET, pargv);
			syslog(LOG_ERR, "exec of %s failed", PATH_IPSET);
			_exit(1);
		default: break;
	}
	waitpid(pid, &s, 0);

	pargv[3] = (char*)ipsrc;
	if (add) {
		pargv[1] = "-A";
		syslog(LOG_INFO, "Adding %s to authorized user table.", ipsrc);
	} else {
		pargv[1] = "-D";
		syslog(LOG_INFO, "Removing %s from authorized user table.", ipsrc);
	}
	switch(pid = fork()) {
		case -1:
			syslog(LOG_ERR, "fork failed");
			goto error;
		case 0: /* This is the child process - execute program*/
			gid = getgid();
			if (setregid(gid, gid) == -1) {
				syslog(LOG_ERR, "setregid: %s", strerror(errno));
			}
			execvp(PATH_IPSET, pargv);
			syslog(LOG_ERR, "exec of %s failed", PATH_IPSET);
			_exit(1);
		default: break; /* this is the parent process, continue */
	}
	waitpid(pid, &s, 0);
	return(0);
error:
	return(-1);
}

static void need_death(int signo) {
	want_death = 1; /* Signal through a variable */
}

static void do_death(int active) {
	int ret = 0;
	if (active) {
		change_filter(0, luser, ipsrc);
		change_table(0, ipsrc);
		syslog(LOG_INFO, "User %s@%s no longer authenticated.", luser, ipsrc);
		/* TODO: kill states */
	}

	if (pidfile[0] && pidfd != -1)
		if (unlink(pidfile) == -1)
			syslog(LOG_ERR, "could not unlink (%s) (%m)",pidfile);
	exit(0);

}
