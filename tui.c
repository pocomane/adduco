
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>

enum {
	KEY_NONE  = 0,
	KEY_UP,
	KEY_DOWN,
	KEY_ENTER,
	KEY_QUIT,
	KEY_KILL,
	KEY_CREATE,
};

static struct termios orig_term;
static int raw_active = 0;

/* restore the terminal to its original settings (idempotent) */
static void tui_restore_term(void) {
	if (raw_active) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
		raw_active = 0;
	}
	/* make sure the cursor is visible again */
	fputs("\033[?25h", stdout);
	fflush(stdout);
}

/* switch the terminal into raw mode so we can read single key presses */
static void tui_enter_raw(void) {
	if (tcgetattr(STDIN_FILENO, &orig_term) == -1)
		return;
	struct termios t = orig_term;
	t.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON|IXOFF);
	t.c_oflag &= ~(OPOST);
	t.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	t.c_cflag &= ~(CSIZE|PARENB);
	t.c_cflag |= CS8;
	t.c_cc[VMIN]  = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == -1)
		return;
	raw_active = 1;
}

/* read a single byte from stdin, waiting at most timeout_ms (or forever if < 0).
 * returns the byte or -1 on timeout / error / EOF. */
static int tui_read_byte(int timeout_ms) {
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	struct timeval tv, *ptv = NULL;
	if (timeout_ms >= 0) {
		tv.tv_sec  = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		ptv = &tv;
	}
	if (select(STDIN_FILENO + 1, &fds, NULL, NULL, ptv) <= 0)
		return -1;
	unsigned char b;
	if (read(STDIN_FILENO, &b, 1) != 1)
		return -1;
	return b;
}

/* read and decode the next key press */
static int tui_read_key(void) {
	int c = tui_read_byte(-1);
	if (c < 0)
		return KEY_QUIT;
	if (c == '\n' || c == '\r')
		return KEY_ENTER;
	if (c == 'q' || c == 'Q')
		return KEY_QUIT;
	if (c == 'd' || c == 'D')
		return KEY_KILL;
	if (c == 'c' || c == 'C')
		return KEY_CREATE;
	if (c == 'j' || c == 'J')
		return KEY_DOWN;
	if (c == 'k' || c == 'K')
		return KEY_UP;
	if (c != 27)
		return KEY_NONE;

	/* an escape sequence (e.g. arrow keys) starts with ESC [ <letter> */
	int b = tui_read_byte(50);
	if (b < 0)
		return KEY_QUIT;   /* lone ESC -> quit */
	if (b != '[')
		return KEY_NONE;
	b = tui_read_byte(50);
	if (b < 0)
		return KEY_NONE;
	switch (b) {
	case 'A': return KEY_UP;
	case 'B': return KEY_DOWN;
	default:  return KEY_NONE;
	}
}

static int tui_clear_for_lines(int lines) {
	struct winsize ws;
	int cols = 80, rows = 25;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
		if (ws.ws_row) rows = ws.ws_row;
		if (ws.ws_col) cols = ws.ws_col;
	}
	fputs("\033[2J\033[H", stdout);
	for (int i = lines; i < rows; i++)
		fputs("\033[0m \r\n", stdout);
	return lines;
}

/* redraw the whole menu, keeping the selection visible (simple viewport) */
static void tui_draw(char **names, int count, int sel, int *top, const char *msg) {

	int avail = tui_clear_for_lines(count + 3); /* items + title + help + status/empty line */

	if (avail < 1)
		avail = 1;

	if (sel < *top)
		*top = sel;
	if (sel >= *top + avail)
		*top = sel - avail + 1;
	if (*top < 0)
		*top = 0;

	fputs("\033[0mAbduco - ", stdout);
	if (msg && *msg)
		fprintf(stdout, "%s\r\n", msg);
	else if (count == 0)
		fprintf(stdout, "No active sessions.\r\n");
	else
		fprintf(stdout, "%d session(s):\r\n", count);

	for (int i = 0; i < avail; i++) {
		int idx = *top + i;
		if (idx >= count)
			break;
		fputs(idx == sel ? "\033[7m" : "\033[0m", stdout);
		fputs(idx == sel ? " > " : "   ", stdout);
		const char *name = names[idx];
		fputs(name, stdout);
		fputs(" ", stdout);
		fputs("\033[0m\r\n", stdout);
	}

	fputs("Arrows to move, d to kill, c to create, ENTER to attach, q to quit.\r\n", stdout);

	fflush(stdout);
}

static void session_list_free(char **names, int count) {
	if (!names)
		return;
	for (int i = 0; i < count; i++)
		free(names[i]);
	free(names);
}

/* Collect the names of all active sessions into *names Returns the number of
 * sessions or -1 on error. */
static void session_list(char ***names, int *count) {
	if (*count > 0){
		session_list_free(*names, *count);
	}
	*count = 0;
	char **list = NULL;
	struct session_iterator iter = {0};
	while(iterate_over_sessions(&iter)){
		char **tmp = realloc(list, (*count + 1) * sizeof *tmp);
		if (tmp) {
			list = tmp;
			list[(*count)++] = strdup(iter.namelist[iter.current]->d_name);
		}
	}
	*names = list;
}

/* Ask the user to confirm killing the selected session. Returns true if confirmed.*/
static bool tui_confirm_kill(const char *name) {
	tui_clear_for_lines(2); /* prompt + empty.status line */
	fputs("\033[1mKill session\033[0m '", stdout);
	fputs(name, stdout);
	fputs("' ? [y/N]\r\n", stdout);
	fflush(stdout);
	int c = tui_read_byte(-1);
	return c == 'y' || c == 'Y';
}

/* Read a line of text from the user. Returns a malloc'd, NUL-terminated string
 * on Enter (which may be empty), or NULL if the user pressed ESC to cancel.
 * The terminal is in raw mode, so characters are echoed manually. */
static char *tui_read_line(const char *prompt) {
	tui_clear_for_lines(2); /* prompt + empty.status line */
	fputs("\033[1m", stdout);
	fputs(prompt, stdout);
	fputs("\033[0m", stdout);
	fflush(stdout);

	size_t cap = 64, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	buf[0] = '\0';

	int c;
	for (;;) {
		c = tui_read_byte(-1);
		if (c < 0)
			break;                 /* EOF / error -> cancel */
		if (c == 27) {             /* ESC: cancel, drain any escape sequence */
			for (;;) {
				int d = tui_read_byte(0);
				if (d < 0)
					break;
			}
			break;
		}
		if (c == '\n' || c == '\r')
			break;                /* finish the line */
		if (c == 127 || c == '\b' || c == 8) {  /* backspace / delete */
			if (len > 0) {
				len--;
				buf[len] = '\0';
				fputs("\b \b", stdout);
				fflush(stdout);
			}
			continue;
		}
		if (c < 32)                /* ignore other control characters */
			continue;
		if (len + 1 >= cap) {
			size_t ncap = cap * 2;
			char *tmp = realloc(buf, ncap);
			if (!tmp)
				break;
			buf = tmp;
			cap = ncap;
		}
		buf[len++] = (char)c;
		buf[len] = '\0';
		putchar(c);
		fflush(stdout);
	}

	if (c == 27 || c < 0) {        /* cancelled */
		free(buf);
		return NULL;
	}
	return buf;
}

/* Prompt the user for a command to run and a session name, then create a new
 * session. Pressing ESC at any prompt cancels and returns to the session list.
 * If a session with the given name already exists, an error is shown. */
static void tui_create_session(char **names, int count, int sel, int *top) {
	const char *msg;
	char *argv[4];
	char *cmd = tui_read_line("Command to run (ESC to cancel): ");
	if (!cmd) {
		msg = "Create cancelled.";
		goto out;
	}
	if (cmd[0] == '\0') {
		free(cmd);
		msg = "Empty command, create aborted.";
		goto out;
	}

	char *name = tui_read_line("Session name (ESC to cancel): ");
	if (!name) {
		free(cmd);
		msg = "Create cancelled.";
		goto out;
	}
	if (name[0] == '\0') {
		free(cmd);
		free(name);
		msg = "Empty name, create aborted.";
		goto out;
	}

	if (session_exists(name)) {
		free(cmd);
		free(name);
		msg = "Session already exists.";
		goto out;
	}

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = cmd;
	argv[3] = NULL;
	if (create_session(name, argv))
		msg = "Session created.";
	else
		msg = "Could not create session.";
	free(cmd);
	free(name);

out:
	tui_draw(names, count, sel, top, msg);
	tui_read_byte(800);
}

void tui_main(void) {
	char **names = NULL;
	int count = 0;
	int sel = 0;
	int top = 0;

	tui_enter_raw();
	atexit(tui_restore_term);

	for (;;) {
		session_list(&names, &count);
		if (count < 0)
			count = 0;
		if (sel >= count)
			sel = count > 0 ? count - 1 : 0;
		if (sel < 0)
			sel = 0;

		tui_draw(names, count, sel, &top, NULL);

		int k = tui_read_key();
		if (k == KEY_QUIT) {
			break;
		} else if (k == KEY_UP) {
			if (sel > 0)
				sel--;
		} else if (k == KEY_DOWN) {
			if (sel < count - 1)
				sel++;
		} else if (k == KEY_ENTER) {
			if (count > 0 && sel < count) {
				char *name = strdup(names[sel]);
				session_list_free(names, count);
				names = NULL;
				count = 0;
				tui_restore_term();
				attach_session(name, false);
				free(name);
				/* the attach call restored the terminal, re-enter raw mode
				 * so we can show the menu again once it returns */
				tui_enter_raw();
				sel = 0;
				top = 0;
			}
		} else if (k == KEY_KILL) {
			if (count > 0 && sel < count) {
				const char *name = names[sel];
				if (tui_confirm_kill(name)) {
					pid_t pid = session_exists(name);
					if (!pid)
						tui_draw(names, count, sel, &top, "Session not found.");
					else if (kill(pid, SIGTERM) == -1)
						tui_draw(names, count, sel, &top, "Could not kill session.");
					else
						tui_draw(names, count, sel, &top, "Session killed.");
					tui_read_byte(800);
				} else {
					tui_draw(names, count, sel, &top, "Kill aborted.");
					tui_read_byte(800);
				}
				sel = 0;
				top = 0;
			}
		} else if (k == KEY_CREATE) {
			tui_create_session(names, count, sel, &top);
		}
	}

	tui_restore_term();
	session_list_free(names, count);
}

