
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
	if (c == 'k' || c == 'K')
		return KEY_KILL;
		return KEY_UP;
	if (c == 'j')
		return KEY_DOWN;
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

/* redraw the whole menu, keeping the selection visible (simple viewport) */
static void tui_draw(char **names, int count, int sel, int *top, const char *msg) {
	struct winsize ws;
	int rows = 25, cols = 80;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
		if (ws.ws_row) rows = ws.ws_row;
		if (ws.ws_col) cols = ws.ws_col;
	}

	int header = 2;                 /* title + help line */
	int avail  = rows - header - 1; /* reserve a status line */
	if (avail < 1)
		avail = 1;

	if (sel < *top)
		*top = sel;
	if (sel >= *top + avail)
		*top = sel - avail + 1;
	if (*top < 0)
		*top = 0;

	fputs("\033[2J\033[H", stdout);
	fputs("\033[1mabduco\033[0m \033[1m- interactive session selector\033[0m\r\n", stdout);
	fputs("Arrow keys or j to move, k to kill, ENTER to attach, q to quit.\r\n", stdout);

	for (int i = 0; i < avail; i++) {
		int idx = *top + i;
		if (idx >= count)
			break;
		fputs(idx == sel ? "\033[7m" : "\033[0m", stdout);
		fputs("  ", stdout);
		const char *name = names[idx];
		int len = strlen(name);
		int max = cols - 2;
		if (max < 0)
			max = 0;
		if (len <= max) {
			fputs(name, stdout);
			for (int p = len; p < max; p++)
				putchar(' ');
		} else {
			fwrite(name, 1, max, stdout);
		}
		fputs("\033[0m\r\n", stdout);
	}

	for (int i = count - *top; i < avail; i++)
		fputs("\033[0m \r\n", stdout);

	fputs("\033[0m", stdout);
	if (msg && *msg)
		fprintf(stdout, "%s\r\n", msg);
	else if (count == 0)
		fprintf(stdout, "No active sessions.\r\n");
	else
		fprintf(stdout, "%d session(s), current: %d/%d\r\n", count, sel + 1, count);
	fflush(stdout);
}

/* Collect the names of all active sessions into *names (caller must free using
 * session_list_free). Returns the number of sessions or -1 on error. */
static int session_list(char ***names) {
	if (!create_socket_dir(&sockaddr))
		return -1;
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof cwd))
		return -1;
	if (chdir(sockaddr.sun_path) == -1)
		return -1;
	struct dirent **namelist;
	int n = scandir(sockaddr.sun_path, &namelist, session_filter, session_comparator);
	if (n < 0) {
		chdir(cwd);
		return -1;
	}
	char **list = NULL;
	int count = 0;
	for (int i = 0; i < n; i++) {
		struct stat sb;
		if (stat(namelist[i]->d_name, &sb) == 0 && S_ISSOCK(sb.st_mode)) {
			pid_t pid = 0;
			char *local = strstr(namelist[i]->d_name, server.host);
			if (local) {
				*local = '\0'; /* truncate hostname if we are local */
				if (!(pid = session_exists(namelist[i]->d_name)))
					goto next;
			}
			char **tmp = realloc(list, (count + 1) * sizeof *tmp);
			if (tmp) {
				list = tmp;
				list[count++] = strdup(namelist[i]->d_name);
			}
		}
		next:
		free(namelist[i]);
	}
	free(namelist);
	chdir(cwd);
	*names = list;
	return count;
}

static void session_list_free(char **names, int count) {
	if (!names)
		return;
	for (int i = 0; i < count; i++)
		free(names[i]);
	free(names);
}

/* Attach to an existing session, returning once it is detached or terminated. */
static int tui_attach_session(const char *name) {
	return attach_session(name, false);
}


/* Ask the user to confirm killing the selected session. Returns true if confirmed.*/
static bool tui_confirm_kill(const char *name) {
	fputs("\033[2J\033[H", stdout);
	fputs("\033[1mKill session\033[0m '", stdout);
	fputs(name, stdout);
	fputs("' ? [y/N] ", stdout);
	fflush(stdout);
	int c = tui_read_byte(-1);
	return c == 'y' || c == 'Y';
}

void tui_main(void) {
	char **names = NULL;
	int count = 0;
	int sel = 0;
	int top = 0;
printf(">>>>>>>>>>>>>\n");
	tui_enter_raw();
	atexit(tui_restore_term);

	for (;;) {
		count = session_list(&names);
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
				tui_attach_session(name);
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
    }
	}

	tui_restore_term();
	session_list_free(names, count);
}

