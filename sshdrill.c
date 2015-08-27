/*
 * Copyright (c) 2015 Marco Pfatschbacher <mpf@mailq.de>
 *
 * tty handling code from script(1)
 *
 * Copyright (c) 1980, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 2001 Theo de Raadt
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

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <ctype.h>
#include <err.h>
#ifdef HAVE_PTY_H
#  include <pty.h>
#endif
#ifdef HAVE_UTIL_H
#  include <util.h>
#endif
#ifdef HAVE_UTMP_H
#  include <utmp.h>
#endif
#ifndef HAVE_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

#define ESCAPE_CHAR '~'
#define ESCAPE_STR  "~"
#define MAX_SSH_DEPTH 6
#define PROMPT "\r\nsshdrill> "

int	master, slave;
volatile sig_atomic_t child;
struct	termios tt;
struct termios rtt;

volatile sig_atomic_t dead;
volatile sig_atomic_t sigdeadstatus;
volatile sig_atomic_t flush;

void done(int);
void doshell(void);
void dooutput(void);
void fail(void);
void finish(int);
void handlesigwinch(int);
int  scan_for_escape(ssize_t, char*);
void command_prompt(void);
int  setup_fwding(int, char *);
ssize_t do_write(int, char *, ssize_t);
int	wait_for_str(char *);
int	prepare_fwds(char *, char *, char *, char *);
void	poke_through(char *, char *, char *, char *);

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	struct winsize win;
	char ibuf[BUFSIZ];
	char obuf[BUFSIZ];
	ssize_t cc, off;
	fd_set rfdset;
	int ret;

	(void)tcgetattr(STDIN_FILENO, &tt);
	(void)ioctl(STDIN_FILENO, TIOCGWINSZ, &win);
	if (openpty(&master, &slave, NULL, &tt, &win) == -1)
		err(1, "openpty");

	rtt = tt;
	cfmakeraw(&rtt);
	rtt.c_lflag &= ~ECHO;
	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &rtt);

	bzero(&sa, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = finish;
	(void)sigaction(SIGCHLD, &sa, NULL);

	sa.sa_handler = handlesigwinch;
	sa.sa_flags = SA_RESTART;
	(void)sigaction(SIGWINCH, &sa, NULL);

	child = fork();
	if (child < 0) {
		warn("fork");
		fail();
	}
	if (child == 0) {
		doshell();
	}

	for (;;) {
		FD_ZERO(&rfdset);
		FD_SET(STDIN_FILENO, &rfdset);
		FD_SET(master, &rfdset);
		if (dead)
			break;
		ret = select(master + 1, &rfdset, NULL, NULL, NULL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			else
				err(1, "select");
		}

		if (FD_ISSET(STDIN_FILENO, &rfdset)) {
			cc = read(STDIN_FILENO, ibuf, BUFSIZ);
			if (cc == -1 && errno == EINTR)
				continue;
			if (cc <= 0)
				break;
			if (scan_for_escape(cc, ibuf)) {
				off = 1; /* jump over ~ */
			} else {
				off = 0;
			}
			do_write(master, ibuf + off, cc - off);
		}
		if (FD_ISSET(master, &rfdset)) {
			cc = read(master, obuf, sizeof (obuf));
			if (cc == -1 && errno == EINTR)
				continue;
			if (cc <= 0)
				break;
			do_write(STDOUT_FILENO, obuf, cc);
		}
	}
	done(sigdeadstatus);

	return 1; /* NOTREACHED */
}

void
command_prompt(void)
{
	char orig_fwd[BUFSIZ], first_fwd[BUFSIZ];
	char tunnel_fwd[BUFSIZ], last_fwd[BUFSIZ];
	ssize_t nr;
	char ch, *p, *fwd, buf[BUFSIZ];

	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &tt);

	if (do_write(STDOUT_FILENO, PROMPT, strlen(PROMPT)) <= 0)
		goto abort;

	p = buf;
	while ((nr = read(STDIN_FILENO, &ch, 1)) == 1 &&
	    ch != '\n' && ch != '\r') {
		if (p < buf + (sizeof(buf) - 1)) {
			*p++ = ch;
		}
	}
	*p = '\0';

	p = buf;

	while (isspace((u_char)*p))
		p++;

	if (*p == '\0')
		goto abort;

	if (*p == 'h' || *p == 'H' || *p == '?') {
		char *help =
		    "\rCommands:\n"
		    "      -L[bind_address:]port:host:hostport    "
		    "Request local forward\n"
		    "      -R[bind_address:]port:host:hostport    "
		    "Request remote forward\n"
		    "      -D[bind_address:]port                  "
		    "Request dynamic forward\n";
		do_write(STDOUT_FILENO, help, strlen(help));
		goto abort;
	}
	fwd = p;
	snprintf(orig_fwd, BUFSIZ, "%s\r", fwd);


	if (prepare_fwds(fwd, first_fwd, tunnel_fwd, last_fwd) == 0)
		poke_through(orig_fwd, first_fwd, tunnel_fwd, last_fwd);

 abort:
	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &rtt);
}

int
prepare_fwds(char *cmd, char *first_fwd, char *tunnel_fwd, char *last_fwd)
{

	char type, colcount, i;
	char *s, *tok;
	char buf[BUFSIZ], source[BUFSIZ], dest[BUFSIZ];
	unsigned int lport;

	s = cmd;

	if (*s == '-')
		s++;	/* Skip cmdline '-', if any */

	type = *s;
	if (type != 'L' && type != 'R' && type != 'D')
		goto parse_error;
	s++;

	if (type == 'D') {
		strlcpy(source, s, BUFSIZ);
		tok = s;
		if ((tok = strchr(tok, ':')))
			s = tok + 1;
		if ((lport = strtoul(s, NULL, 10)) == 0)
			goto parse_error;
		snprintf(first_fwd, BUFSIZ, "-L%s:127.0.0.1:%u\r", source, lport);
		snprintf(tunnel_fwd, BUFSIZ, "-L%u:127.0.0.1:%u\r", lport, lport);
		snprintf(last_fwd, BUFSIZ, "-D:%u\r", lport);
		return 0;
	}

	tok = s;
	colcount = 0;
	while ((tok = strchr(tok, ':'))) {
		tok++;
		colcount++;
	}
	if (colcount < 2 || colcount > 3)
		goto parse_error;

	strlcpy(source, s, BUFSIZ);
	tok = s;
	for (i = 0; i < colcount - 2; i++) {
		tok = strchr(tok, ':');
		tok++;
	}
	if (strlcpy(buf, tok, BUFSIZ) >= BUFSIZ)
		goto parse_error;
	tok = strchr(tok, ':');
	*tok = '\0';

	*(source + (tok - s)) = '\0';

	if ((lport = strtoul(s, NULL, 10)) == 0)
		goto parse_error;
	strlcpy(dest, tok + 1, BUFSIZ);

	if (type == 'L') {
		snprintf(first_fwd, BUFSIZ, "-L%s:127.0.0.1:%u\r", source, lport);
		snprintf(tunnel_fwd, BUFSIZ, "-L%u:127.0.0.1:%u\r", lport, lport);
		snprintf(last_fwd, BUFSIZ, "-L%u:%s\r", lport, dest);
	} else if (type == 'R') {
		snprintf(first_fwd, BUFSIZ, "-R%u:%s\r", lport, dest);
		snprintf(tunnel_fwd, BUFSIZ, "-R%u:127.0.0.1:%u\r", lport, lport);
		snprintf(last_fwd, BUFSIZ, "-R%s:127.0.0.1:%u\r", source, lport);
	}

	return 0;

 parse_error:
	fprintf(stderr, "Invalid command.");
	return 1;
}

#define SEND_PROBE 1
#define READ_PROBE 2
#define SEND_CLEANUP 3
#define PROBE_DONE 4

void
poke_through(char *orig_fwd, char *first_fwd, char *tunnel_fwd, char *last_fwd)
{
	fd_set rfdset;
	int ret;
	char obuf[BUFSIZ];
	struct timeval tout;
	int state = SEND_PROBE;
	int escapes_sent = 0;
	int backspaces_sent = 0;
	int escapes_rcvd = 0;
	int nested_sshs = 0;
	int current_hop;

	tout.tv_sec = 1;
	tout.tv_usec = 0;

	for (;;) {
		if (state == SEND_PROBE) {
			if (do_write(master, ESCAPE_STR, 1)) {
				if (++escapes_sent == MAX_SSH_DEPTH)
					state = READ_PROBE;
			}
		}

		if (state == READ_PROBE) {
			FD_ZERO(&rfdset);
			FD_SET(master, &rfdset);
			ret = select(master + 1, &rfdset, NULL, NULL, &tout);
			if (ret < 0 && errno != EINTR)
				err(1, "select");
			if (ret == 0) {
				state = SEND_CLEANUP;
			}
			if (FD_ISSET(master, &rfdset)) {
				ssize_t cc;
				cc = read(master, obuf, sizeof (obuf));
				if (cc == -1 && errno == EINTR)
					continue;
				if (cc <= 0)
					break;
				/*
				 * XXX Check for actual ~ chars
				 * This only works if there's no additional
				 * console output
				 */
				escapes_rcvd += cc;
			}
		}
		if (state == SEND_CLEANUP) {
			ssize_t n = write(master, "", 1);
			if (n == -1 && errno != EAGAIN)
				break;
			if (n == 0)
				break;	/* skip writing */
			if (n > 0) {
				if (++backspaces_sent == MAX_SSH_DEPTH) {
					state = PROBE_DONE;
					break;
				}
			}
		}
	}
	
	if (state == PROBE_DONE) {
		nested_sshs = escapes_sent - escapes_rcvd;
		if (nested_sshs < 0) {
			fprintf(stderr, "Scan failure.\n");
			return;
		}
	}

	if (nested_sshs)
		fprintf(stderr,
		    "Forwarding port through %d ssh sessions.\n", nested_sshs);
	else
		fprintf(stderr, "No ssh sessions found.\n");

	for (current_hop = nested_sshs; current_hop; current_hop--) {
		char *fwd;

		if (current_hop == nested_sshs) {
			if (do_write(master, "\r", 1) <= 0)
				fprintf(stderr, "do_write error");
			fwd = nested_sshs == 1 ? orig_fwd : last_fwd;
		} else if (current_hop > 1)
			fwd = tunnel_fwd;
		else
			fwd = first_fwd;

		if (setup_fwding(current_hop, fwd))
			fprintf(stderr, "setup_fwding error\n");
	}
}

int
setup_fwding(int current_hop, char *fwd)
{

	while (current_hop) {
		if (do_write(master, "~", 1) > 0)
			current_hop--;
		else
			return -1;
	}
	do_write(master, "C", 1);
	if (wait_for_str("ssh>") != 0) {
		fprintf(stderr, "timeout waiting for ssh> prompt\n");
		return -1;
	}
	if (do_write(master, fwd, strlen(fwd)) > 0) {
		wait_for_str(NULL);
		return 0;
	} else
		return -1;
}

int
wait_for_str(char *searchstr)
{
	fd_set rfdset;
	int ret;
	unsigned int cc = 0;
	ssize_t n;
	char obuf[BUFSIZ];
	struct timeval tout;

	tout.tv_sec = 1;
	tout.tv_usec = 0;

	while (cc < sizeof(obuf)) {
		FD_ZERO(&rfdset);
		FD_SET(master, &rfdset);
		ret = select(master + 1, &rfdset, NULL, NULL, &tout);
		if (ret < 0 && errno != EINTR)
			err(1, "select");
		if (ret == 0) {
			return 1;
		}
		if (FD_ISSET(master, &rfdset)) {
			n = read(master, obuf + cc, sizeof (obuf) - cc);
			if (n == -1 && errno == EINTR)
				continue;
			do_write(STDOUT_FILENO, obuf + cc, n);

			if (searchstr && strstr(obuf, searchstr))
				return 0;
			cc += n;
		}
	}
	return 1;
}

ssize_t
do_write(int fd, char *buf, ssize_t cc) {

	ssize_t off = 0;
	ssize_t n = 0;

	while (off < cc) {
		n = write(fd, buf + off, cc - off);
		if (n == -1 && errno != EAGAIN)
			return n;
		if (n == 0)
			return n;
		if (n > 0) {
			off += n;
		}
	}
	return off;
}

int
scan_for_escape(ssize_t cc, char *ibuf)
{
	static int got_newline = 0;
	static int got_escape = 0;
	int i;

	for (i = 0; i < cc; i++) {
		if (got_escape && ibuf[i] == 'C') {
			got_newline = got_escape = 0;
			command_prompt();
			return 1;
		} else if (got_newline && ibuf[i] == ESCAPE_CHAR) {
			got_escape = 1;
			got_newline = 0;
			return 1;
		} else if (ibuf[i] == '\r') {
			got_newline = 1;
			got_escape = 0;
		} else {
			got_newline = got_escape = 0;
		}
	}
	return 0;
}

/* ARGSUSED */
void
handlesigwinch(int signo)
{
	int save_errno = errno;
	struct winsize win;
	pid_t pgrp;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) != -1) {
		ioctl(slave, TIOCSWINSZ, &win);
		if (ioctl(slave, TIOCGPGRP, &pgrp) != -1)
			killpg(pgrp, SIGWINCH);
	}
	errno = save_errno;
}

/* ARGSUSED */
void
finish(int signo)
{
	int save_errno = errno;
	int status, e = 1;
	pid_t pid;

	while ((pid = wait3(&status, WNOHANG, 0)) > 0) {
		if (pid == (pid_t)child) {
			if (WIFEXITED(status))
				e = WEXITSTATUS(status);
		}
	}
	dead = 1;
	sigdeadstatus = e;
	errno = save_errno;
}

void
doshell(void)
{
	char *shell;

	shell = getenv("SHELL");
	if (shell == NULL)
		shell = _PATH_BSHELL;

	(void)close(master);
	login_tty(slave);
	execl(shell, shell, "-il", (char *)NULL);
	warn("%s", shell);
	fail();
}

void
fail(void)
{

	(void)kill(0, SIGTERM);
	done(1);
}

void
done(int eval)
{
	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &tt);
	exit(eval);
}
