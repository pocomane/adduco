//
// Copyright (c) 2013-2018 Marc André Tanner <mat at brain-dump.org>,
// 2026 Mimmo Mane <github/pocomane>.
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

// --------------------------------------------------------------------------------
// Core header inclusion

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <libgen.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#if defined(__linux__) || defined(__CYGWIN__)
# include <pty.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
# include <libutil.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
# include <util.h>
#endif

#if defined CTRL && defined _AIX
  #undef CTRL
#endif
#ifndef CTRL
  #define CTRL(k)   ((k) & 0x1F)
#endif

// --------------------------------------------------------------------------------
// Configuration

#ifndef VERSION
#define VERSION "v0.develop"
#endif

// default shell to be used to run the commands
#define ADDUCO_SHELL "sh"
// default command to execute if non is given and $ADDUCO_CMD is unset
#define ADDUCO_CMD "sh"
// environment variable for user provided shell to use to execute commands
#define ADDUCO_SHELL_ENV "ADDUCO_CMD_SHELL"
// environment variable for user provided command to execute by default
#define ADDUCO_CMD_ENV "ADDUCO_CMD"
// default detach key, can be overriden at run time using -e option
static char KEY_DETACH = CTRL('\\');
// redraw key to send a SIGWINCH signal to underlying process
// (set to 0 to disable the redraw key)
static char KEY_REDRAW = 0;
// Where to place the "adduco" directory storing all session socket files.
// The first directory to succeed is used.
static struct Dir {
	enum {
		SKIP,         /* placeholder for command line argument */
		PERSONAL_ENV, /* environment variable pointing to personal folder (/adduco will be added) */
		COMMON_ENV,   /* environment variable pointing to folder shared among users (/adduco/username will be added_ */
		PATH,         /* absolute path to a directory (nothing will be added) */
	} mode;
	char *reference;
} socket_dirs[] = {
	{ SKIP,         ""},
	{ COMMON_ENV,   "ADDUCO_SOCKET_DIR"},
	{ PERSONAL_ENV, "HOME"},
	{ COMMON_ENV,   "TMPDIR"},
	{ PATH,         "/tmp/adduco"},
};

// --------------------------------------------------------------------------------
// Core definitions

#if defined(_AIX)
# include "forkpty-aix.c"
#elif defined(__sun)
# include "forkpty-sunos.c"
#endif

#define countof(arr) (sizeof(arr) / sizeof((arr)[0]))

struct {
  bool has_term, quiet, passthrough;
} options;

enum PacketType {
	MSG_CONTENT = 0,
	MSG_ATTACH  = 1,
	MSG_DETACH  = 2,
	MSG_RESIZE  = 3,
	MSG_EXIT    = 4,
	MSG_PID     = 5,
	MSG_RENAME  = 6,
	MSG_SIGNAL  = 7,
};

typedef struct {
	uint32_t type;
	uint32_t len;
	union {
		char msg[4096 - 2*sizeof(uint32_t)];
		struct {
			uint16_t rows;
			uint16_t cols;
		} ws;
		uint32_t i;
		uint64_t l;
	} u;
} Packet;

typedef struct Client Client;
struct Client {
	int socket;
	enum {
		STATE_CONNECTED,
		STATE_ATTACHED,
		STATE_DETACHED,
		STATE_DISCONNECTED,
	} state;
	bool need_resize;
	enum {
		CLIENT_READONLY = 1 << 0,
		CLIENT_LOWPRIORITY = 1 << 1,
	} flags;
	Client *next;
};

typedef struct {
	Client *clients;
	int socket;
	Packet pty_output;
	int pty;
	int exit_status;
	struct termios term;
	struct winsize winsize;
	pid_t pid;
	volatile sig_atomic_t running;
	const char *name;
	char session_name[255];
	char host[255];
	bool read_pty;
} Server;

static Server server = { .running = true, .exit_status = -1, .host = "@localhost" };
static Client client;
static struct termios orig_term, cur_term;
static bool alternate_buffer;

static struct sockaddr_un sockaddr = {
	.sun_family = AF_UNIX,
};

static bool set_socket_name(struct sockaddr_un *sockaddr, const char *name);

// --------------------------------------------------------------------------------
// Debug

static void print_packet(const char *prefix, Packet *pkt) {
	static const char *msgtype[] = {
		[MSG_CONTENT] = "CONTENT",
		[MSG_ATTACH]  = "ATTACH",
		[MSG_DETACH]  = "DETACH",
		[MSG_RESIZE]  = "RESIZE",
		[MSG_EXIT]    = "EXIT",
		[MSG_PID]     = "PID",
	};
	const char *type = "UNKNOWN";
	if (pkt->type < countof(msgtype) && msgtype[pkt->type])
		type = msgtype[pkt->type];

	fprintf(stderr, "%s: %s ", prefix, type);
	switch (pkt->type) {
	case MSG_CONTENT:
		fprintf(stderr, " hex: ");
		for (int c = 0; c < pkt->len; c += 1)
			fprintf(stderr, "%02x", pkt->u.msg[c]);
		fprintf(stderr, " char: ");
		fwrite(pkt->u.msg, pkt->len, 1, stderr);
		break;
	case MSG_RESIZE:
		fprintf(stderr, "%"PRIu16"x%"PRIu16, pkt->u.ws.cols, pkt->u.ws.rows);
		break;
	case MSG_ATTACH:
		fprintf(stderr, "readonly: %d low-priority: %d",
			pkt->u.i & CLIENT_READONLY,
			pkt->u.i & CLIENT_LOWPRIORITY);
		break;
	case MSG_EXIT:
		fprintf(stderr, "status: %"PRIu32, pkt->u.i);
		break;
	case MSG_PID:
		fprintf(stderr, "pid: %"PRIu32, pkt->u.i);
		break;
	default:
		fprintf(stderr, "len: %"PRIu32, pkt->len);
		break;
	}
	fprintf(stderr, "\n");
}

enum InfoType {
	ERROR = 1,
	ERROR_LIB,
	INFO,
	DEBUG,
	DEBUG_PACKET,
};

static void info(enum InfoType type, const char *str, ...) {
	if (!type) return;
#ifdef NDEBUG
	if (type == DEBUG) return;
	if (type == DEBUG_PACKET) return;
#endif
	char* liberr = NULL;
	if (ERROR_LIB == type)
		liberr = (char*) strerror(errno);
	va_list ap;
	va_start(ap, str);
#ifndef NDEBUG
	if (type == DEBUG_PACKET){
		print_packet(str, va_arg(ap, Packet *));
		return;
	}
#endif
	if (str && !options.quiet) {
		fprintf(stderr, "%s: %s: ", server.name, server.session_name);
		vfprintf(stderr, str, ap);
		if (liberr)
			fprintf(stderr, " - %s", liberr);
		fprintf(stderr, "\r\n");
		fflush(stderr);
	}
	va_end(ap);
}

static void die(const char *s) {
	info(ERROR_LIB, "%s", s);
	exit(EXIT_FAILURE);
}

// --------------------------------------------------------------------------------
// Packet IO

static inline size_t packet_header_size() {
	return offsetof(Packet, u);
}

static size_t packet_size(Packet *pkt) {
	return packet_header_size() + pkt->len;
}

static ssize_t write_all(int fd, const char *buf, size_t len) {
	info(DEBUG, "write_all(%d)\n", len);
	ssize_t ret = len;
	while (len > 0) {
		ssize_t res = write(fd, buf, len);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			return -1;
		}
		if (res == 0)
			return ret - len;
		buf += res;
		len -= res;
	}
	return ret;
}

static ssize_t read_all(int fd, char *buf, size_t len) {
	info(DEBUG, "read_all(%d)\n", len);
	ssize_t ret = len;
	while (len > 0) {
		ssize_t res = read(fd, buf, len);
		if (res < 0) {
			if (errno == EWOULDBLOCK)
				return ret - len;
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return -1;
		}
		if (res == 0)
			return ret - len;
		buf += res;
		len -= res;
	}
	return ret;
}

static bool send_packet(int socket, Packet *pkt) {
	size_t size = packet_size(pkt);
	if (size > sizeof(*pkt))
		return false;
	return write_all(socket, (char *)pkt, size) == size;
}

static bool recv_packet(int socket, Packet *pkt) {
	ssize_t len = read_all(socket, (char*)pkt, packet_header_size());
	if (len <= 0 || len != packet_header_size())
		return false;
	if (pkt->len > sizeof(pkt->u.msg)) {
		pkt->len = 0;
		return false;
	}
	if (pkt->len > 0) {
		len = read_all(socket, pkt->u.msg, pkt->len);
		if (len <= 0 || len != pkt->len)
			return false;
	}
	return true;
}

// --------------------------------------------------------------------------------
// Client

static void client_sigwinch_handler(int sig) {
	client.need_resize = true;
}

static bool client_send_packet(Packet *pkt) {
	info(DEBUG_PACKET, "client-send:", pkt);
	if (send_packet(server.socket, pkt))
		return true;
	info(DEBUG, "FAILED\n");
	server.running = false;
	return false;
}

static bool client_recv_packet(Packet *pkt) {
	if (recv_packet(server.socket, pkt)) {
		info(DEBUG_PACKET, "client-recv:", pkt);
		return true;
	}
	info(DEBUG, "client-recv: FAILED\n");
	server.running = false;
	return false;
}

static void client_restore_terminal(void) {
	if (!options.has_term)
		return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
	if (alternate_buffer) {
		printf("\033[?25h\033[?1049l");
		fflush(stdout);
		alternate_buffer = false;
	}
}

static void client_setup_terminal(void) {
	if (!options.has_term)
		return;
	atexit(client_restore_terminal);

	cur_term = orig_term;
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON|IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = _POSIX_VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &cur_term);

	if (!alternate_buffer) {
		printf("\033[?1049h\033[H");
		fflush(stdout);
		alternate_buffer = true;
	}
}

static int client_mainloop(void) {
	sigset_t emptyset, blockset;
	sigemptyset(&emptyset);
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGWINCH);
	sigprocmask(SIG_BLOCK, &blockset, NULL);

	client.need_resize = true;
	Packet pkt = {
		.type = MSG_ATTACH,
		.u.i = client.flags,
		.len = sizeof(pkt.u.i),
	};
	client_send_packet(&pkt);

	while (server.running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(server.socket, &fds);

		if (client.need_resize) {
			struct winsize ws;
			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
				Packet pkt = {
					.type = MSG_RESIZE,
					.u = { .ws = { .rows = ws.ws_row, .cols = ws.ws_col } },
					.len = sizeof(pkt.u.ws),
				};
				if (client_send_packet(&pkt))
					client.need_resize = false;
			}
		}

		if (pselect(server.socket+1, &fds, NULL, NULL, NULL, &emptyset) == -1) {
			if (errno == EINTR)
				continue;
			die("client-mainloop");
		}

		if (FD_ISSET(server.socket, &fds)) {
			Packet pkt;
			if (client_recv_packet(&pkt)) {
				switch (pkt.type) {
				case MSG_CONTENT:
					if (!options.passthrough)
						write_all(STDOUT_FILENO, pkt.u.msg, pkt.len);
					break;
				case MSG_RESIZE:
					client.need_resize = true;
					break;
				case MSG_EXIT:
					client_send_packet(&pkt);
					close(server.socket);
					return pkt.u.i;
				}
			}
		}

		if (FD_ISSET(STDIN_FILENO, &fds)) {
			Packet pkt = { .type = MSG_CONTENT };
			ssize_t len = read(STDIN_FILENO, pkt.u.msg, sizeof(pkt.u.msg));
			if (len == -1 && errno != EAGAIN && errno != EINTR)
				die("client-stdin");
			if (len > 0) {
				info(DEBUG, "client-stdin: %c\n", pkt.u.msg[0]);
				pkt.len = len;
				if (KEY_REDRAW && pkt.u.msg[0] == KEY_REDRAW) {
					client.need_resize = true;
				} else if (pkt.u.msg[0] == KEY_DETACH) {
					pkt.type = MSG_DETACH;
					pkt.len = 0;
					client_send_packet(&pkt);
					close(server.socket);
					return -1;
				} else if (!(client.flags & CLIENT_READONLY)) {
					client_send_packet(&pkt);
				}
			} else if (len == 0) {
				info(DEBUG, "client-stdin: EOF\n");
				return -1;
			}
		}
	}

	return -EIO;
}

// --------------------------------------------------------------------------------
// Server

#define FD_SET_MAX(fd, set, maxfd) do { \
		FD_SET(fd, set);        \
		if (fd > maxfd)         \
			maxfd = fd;     \
	} while (0)

static Client *client_malloc(int socket) {
	Client *c = calloc(1, sizeof(Client));
	if (!c)
		return NULL;
	c->socket = socket;
	return c;
}

static void client_free(Client *c) {
	if (c && c->socket > 0)
		close(c->socket);
	free(c);
}

static void server_sink_client() {
	if (!server.clients || !server.clients->next)
		return;
	Client *target = server.clients;
	server.clients = target->next;
	Client *dst = server.clients;
	while (dst->next)
		dst = dst->next;
	target->next = NULL;
	dst->next = target;
}

static void server_mark_socket_exec(bool exec, bool usr) {
	struct stat sb;
	if (stat(sockaddr.sun_path, &sb) == -1)
		return;
	mode_t mode = sb.st_mode;
	mode_t flag = usr ? S_IXUSR : S_IXGRP;
	if (exec)
		mode |= flag;
	else
		mode &= ~flag;
	chmod(sockaddr.sun_path, mode);
}

static int server_create_socket(const char *name) {
	if (!set_socket_name(&sockaddr, name))
		return -1;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
		return -1;
	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	mode_t mask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
	int r = bind(fd, (struct sockaddr*)&sockaddr, socklen);
	umask(mask);

	if (r == -1) {
		close(fd);
		return -1;
	}

	if (listen(fd, 5) == -1) {
		unlink(sockaddr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}

static int server_set_socket_non_blocking(int sock) {
	int flags;
	if ((flags = fcntl(sock, F_GETFL, 0)) == -1)
		flags = 0;
    	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static bool server_read_pty(Packet *pkt) {
	pkt->type = MSG_CONTENT;
	ssize_t len = read(server.pty, pkt->u.msg, sizeof(pkt->u.msg));
	if (len > 0)
		pkt->len = len;
	else if (len == 0)
		server.running = false;
	else if (len == -1 && errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
		server.running = false;
	info(DEBUG_PACKET, "server-read-pty:", pkt);
	return len > 0;
}

static bool server_write_pty(Packet *pkt) {
	info(DEBUG_PACKET, "server-write-pty:", pkt);
	size_t size = pkt->len;
	if (write_all(server.pty, pkt->u.msg, size) == size)
		return true;
	info(DEBUG, "FAILED\n");
	server.running = false;
	return false;
}

static bool server_recv_packet(Client *c, Packet *pkt) {
	if (recv_packet(c->socket, pkt)) {
		info(DEBUG_PACKET, "server-recv:", pkt);
		return true;
	}
	info(DEBUG, "server-recv: FAILED\n");
	c->state = STATE_DISCONNECTED;
	return false;
}

static bool server_send_packet(Client *c, Packet *pkt) {
	info(DEBUG_PACKET, "server-send:", pkt);
	if (send_packet(c->socket, pkt))
		return true;
	info(DEBUG, "FAILED\n");
	c->state = STATE_DISCONNECTED;
	return false;
}

static void server_pty_died_handler(int sig) {
	int errsv = errno;
	pid_t pid;

	while ((pid = waitpid(-1, &server.exit_status, WNOHANG)) != 0) {
		if (pid == -1)
			break;
		server.exit_status = WEXITSTATUS(server.exit_status);
		server_mark_socket_exec(true, false);
	}

	info(DEBUG, "server pty died: %d\n", server.exit_status);
	errno = errsv;
}

static void server_sigterm_handler(int sig) {
	exit(EXIT_FAILURE); /* invoke atexit handler */
}

static Client *server_accept_client(void) {
	int newfd = accept(server.socket, NULL, NULL);
	if (newfd == -1 || server_set_socket_non_blocking(newfd) == -1)
		goto error;
	Client *c = client_malloc(newfd);
	if (!c)
		goto error;
	if (!server.clients)
		server_mark_socket_exec(true, true);
	c->socket = newfd;
	c->state = STATE_CONNECTED;
	c->next = server.clients;
	server.clients = c;
	server.read_pty = true;

	Packet pkt = {
		.type = MSG_PID,
		.len = sizeof pkt.u.l,
		.u.l = getpid(),
	};
	server_send_packet(c, &pkt);

	return c;
error:
	if (newfd != -1)
		close(newfd);
	return NULL;
}

static void server_sigusr1_handler(int sig) {
	int socket = server_create_socket(server.session_name);
	if (socket != -1) {
		if (server.socket)
			close(server.socket);
		server.socket = socket;
	}
}

static bool server_rename_session(const char *newname) {
	char oldpath[sizeof(sockaddr.sun_path)];
	strncpy(oldpath, sockaddr.sun_path, sizeof(oldpath) - 1);
	oldpath[sizeof(oldpath) - 1] = (char)0;
	
	// create the new communication socket at the new path
	int newfd = server_create_socket(newname);
	if (newfd == -1)
		return false;

	if (server.socket > 0)
		close(server.socket);
	server.socket = newfd;
	unlink(oldpath);

	// keep session name in sync for SIGUSR1 recreation
	strncpy(server.session_name, newname, sizeof(server.session_name));
	server.session_name[sizeof(server.session_name)-1] = '\0';
	return true;
}

static void server_atexit_handler(void) {
	unlink(sockaddr.sun_path);
}

static void server_mainloop(void) {
	atexit(server_atexit_handler);
	fd_set new_readfds, new_writefds;
	FD_ZERO(&new_readfds);
	FD_ZERO(&new_writefds);
	FD_SET(server.socket, &new_readfds);
	int new_fdmax = server.socket;
	bool exit_packet_delivered = false;

	if (server.read_pty)
		FD_SET_MAX(server.pty, &new_readfds, new_fdmax);

	while (server.clients || !exit_packet_delivered) {
		int fdmax = new_fdmax;
		fd_set readfds = new_readfds;
		fd_set writefds = new_writefds;
		FD_SET_MAX(server.socket, &readfds, fdmax);

		if (select(fdmax+1, &readfds, &writefds, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			die("server-mainloop");
		}

		FD_ZERO(&new_readfds);
		FD_ZERO(&new_writefds);
		new_fdmax = server.socket;

		bool pty_data = false;

		Packet server_packet, client_packet;

		if (FD_ISSET(server.socket, &readfds))
			server_accept_client();

		if (FD_ISSET(server.pty, &readfds))
			pty_data = server_read_pty(&server_packet);

		for (Client **prev_next = &server.clients, *c = server.clients; c;) {
			if (FD_ISSET(c->socket, &readfds) && server_recv_packet(c, &client_packet)) {
				switch (client_packet.type) {
				case MSG_CONTENT:
					server_write_pty(&client_packet);
					break;
				case MSG_ATTACH:
					c->flags = client_packet.u.i;
					if (c->flags & CLIENT_LOWPRIORITY)
						server_sink_client();
					break;
				case MSG_RESIZE:
					c->state = STATE_ATTACHED;
					if (!(c->flags & CLIENT_READONLY) && c == server.clients) {
						info(DEBUG, "server-ioct: TIOCSWINSZ\n");
						struct winsize ws = { 0 };
						ws.ws_row = client_packet.u.ws.rows;
						ws.ws_col = client_packet.u.ws.cols;
						ioctl(server.pty, TIOCSWINSZ, &ws);
					}
					pid_t fg = tcgetpgrp(server.pty);
					if (fg > 0)
						// Send SIGWINCH to the foreground process group of the pty. This
						// is needed when the supervised (e.g. sh) launched an
						// interactive program (e.g. vim)
						kill(-fg, SIGWINCH);
					else
						kill(-server.pid, SIGWINCH);
					break;
				case MSG_EXIT:
					exit_packet_delivered = true;
					// fall through
				case MSG_DETACH:
					c->state = STATE_DISCONNECTED;
					break;
				case MSG_RENAME: {
					bool ok = server_rename_session((const char *)client_packet.u.msg);
					Packet ack = {
						.type = MSG_RENAME,
						.len = sizeof(ack.u.i),
						.u.i = ok ? 1 : 0,
					};
					server_send_packet(c, &ack);
					break;
				}
				case MSG_SIGNAL:
					kill(getpid(), client_packet.u.i);
					break;
				default: /* ignore package */
					break;
				}
			}

			if (c->state == STATE_DISCONNECTED) {
				bool first = (c == server.clients);
				Client *t = c->next;
				client_free(c);
				*prev_next = c = t;
				if (first && server.clients) {
					Packet pkt = {
						.type = MSG_RESIZE,
						.len = 0,
					};
					server_send_packet(server.clients, &pkt);
				} else if (!server.clients) {
					server_mark_socket_exec(false, true);
				}
				continue;
			}

			FD_SET_MAX(c->socket, &new_readfds, new_fdmax);

			if (pty_data)
				server_send_packet(c, &server_packet);
			if (!server.running) {
				if (server.exit_status != -1) {
					Packet pkt = {
						.type = MSG_EXIT,
						.u.i = server.exit_status,
						.len = sizeof(pkt.u.i),
					};
					if (!server_send_packet(c, &pkt))
						FD_SET_MAX(c->socket, &new_writefds, new_fdmax);
				} else {
					FD_SET_MAX(c->socket, &new_writefds, new_fdmax);
				}
			}
			prev_next = &c->next;
			c = c->next;
		}

		if (server.running && server.read_pty)
			FD_SET_MAX(server.pty, &new_readfds, new_fdmax);
	}

	exit(EXIT_SUCCESS);
}

// --------------------------------------------------------------------------------
// Application functions

static void print_help(void) {
	fprintf(stdout, "%s",
		"\n"
		"Adduco - Version "VERSION" \n"
		"- Written by Marc André Tanner <mat at brain-dump.org>.\n"
		"- Small improvements by Mimmo Mane <github.com/pocomane>\n"
		"\n"
		"This program disassociates a given application from its controlling terminal, providing session attach/detach support similar to screen, tmux, or dtach.\n"
		"\n"
		"A session consists of an server process that spawns a user command in its own pseudo terminal. Each session is named and represented by a Unix domain socket stored in the local filesystem. Clients can connect to the session, and their standard input/output streams are relayed to the supervised command.\n"
		"\n"
		"It operates on the raw I/O byte stream without interpreting terminal escape sequences. Consequently, terminal state is not preserved across sessions. For such functionality, use a utility like dvtm.\n"
		"\n"
		"Synopsis:\n"
		"  adduco\n"
		"  adduco [-s|-h]\n"
		"  adduco -k name\n"
		"  adduco -m newname name]\n"
		"  adduco [-a|-k] [options] name\n"
		"  adduco [-A|-c|-n] [options] name [command [args ...]]\n"
		"\n"
		"If no arguments are provided, the interactive mode starts.\n"
		"\n"
		"Available actions:\n"
		"  -h  Print this help.\n"
		"  -s  Lists all active sessions.\n"
		"  -a  Attach to an existing session.\n"
		"  -A  Attach to an existing session; if it doesn't exist, create and attach.\n"
		"  -c  Create a new session and attach immediately.\n"
		"  -n  Create a new session but do not attach.\n"
		"  -k  Kill an existing session.\n"
		"  -m newname  Rename an existing session.\n"
		"\n"
		"The list of the active session is sorted by creation date, moreover:\n"
		"  *  indicates at least one client is connected.\n"
		"  +  indicates the command terminated while no client was connected; attaching will show its exit status.\n"
		"\n"
		"Options:\n"
		"  -e  detachkey` Set detach key (default: Ctrl+\\).\n"
		"  -f  Force session creation even if a terminated session with the same name exists.\n"
		"  -l  Attach with lowest priority for terminal size control.\n"
		"  -p  Pass stdin content to session (implies -q and -l).\n"
		"  -q  Quiet mode; suppress informative messages.\n"
		"  -r  Read-only mode; ignore user input.\n"
		"\n"
		"Signals:\n"
		"  SIGWINCH  Sent to supervised process when primary client resizes terminal.\n"
		"  SIGUSR1   Recreates the Unix domain socket if deleted.\n"
		"  SIGTERM   Detaches a client.\n"
		"\n"
		"Environment:\n"
		"  "ADDUCO_SHELL_ENV"      Shell to use to run the commands (-c plus the command string will be appended); defaults to '" ADDUCO_SHELL "'.\n"
		"  "ADDUCO_CMD_ENV"      Command to run if none specified; defaults to '" ADDUCO_CMD "'.\n"
		"  ADDUCO_SESSION  Current session name visible to the command.\n"
		"  ADDUCO_SOCKET   Absolute path to the session socket.\n"
		"\n"
		"Session data is stored in the first available of these directories:\n"
		"  $ADDUCO_SOCKET_DIR/adduco\n"
		"  $HOME/.adduco\n"
		"  $TMPDIR/adduco/$USER\n"
		"  /tmp/adduco/$USER\n"
		"\n"
		"If the session name is a relative or absolute path, it is used as-is.\n"
		"\n"
		"Examples:\n"
		"\n"
		"# Start a new session (runs `sh` by default)\n"
		"adduco -c my-session\n"
		"\n"
		"# Detach with Ctrl+\\, then reattach later\n"
		"adduco -a my-session\n"
		"\n"
		"# Start a session with a specific command\n"
		"adduco -c my-session /bin/sh\n"
		"\n"
		"# Use Ctrl+Z as detach key\n"
		"adduco -e ^z -a my-session\n"
		"\n"
		"# Send a command to a session\n"
		"echo make | adduco -a my-session\n"
		"\n"
		"# Interactive input\n"
		"adduco -p my-session\n"
		"make\n"
		"^D\n"
		"\n"
	);
}

static void wrong_usage(void) {
	fprintf(stderr, "%s", "Wrong argument, call with the -h flag for help.\n");
	exit(EXIT_FAILURE);
}

static bool xsnprintf(char *buf, size_t size, const char *fmt, ...) {
	va_list ap;
	if (size > INT_MAX)
		return false;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n == -1)
		return false;
	if (n >= size) {
		errno = ENAMETOOLONG;
		return false;
	}
	return true;
}

static int session_connect(const char *name) {
	int fd;
	struct stat sb;
	if (!set_socket_name(&sockaddr, name) || (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;
	socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr.sun_path) + 1;
	if (connect(fd, (struct sockaddr*)&sockaddr, socklen) == -1) {
		if (errno == ECONNREFUSED && stat(sockaddr.sun_path, &sb) == 0 && S_ISSOCK(sb.st_mode))
			unlink(sockaddr.sun_path);
		errno = 0;
		close(fd);
		return -1;
	}
	return fd;
}

static pid_t session_get_connection_ack() {
	Packet pkt = {0};
	pid_t pid = 0;
	if (client_recv_packet(&pkt) && pkt.type == MSG_PID)
		pid = pkt.u.l;
	return pid;
}

static pid_t session_exists(const char *name) {
	Packet pkt;
	pid_t pid = 0;
	if ((server.socket = session_connect(name)) == -1)
		return pid;
	pid = session_get_connection_ack();
	close(server.socket);
	return pid;
}

static bool session_alive(const char *name) {
	struct stat sb;
	return session_exists(name) &&
	       stat(sockaddr.sun_path, &sb) == 0 &&
	       S_ISSOCK(sb.st_mode) && (sb.st_mode & S_IXGRP) == 0;
}

static bool create_socket_dir(char *path, int path_max_len) {
	struct sockaddr_un s = {
		.sun_family = AF_UNIX,
	};
	static char cache[sizeof(s.sun_path)] = {0};
	if (cache[0]){
		strncpy(path, cache, path_max_len);
		return true;
	}

	struct sockaddr_un *sockaddr = &s;
	int socketfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socketfd == -1)
		return false;

	const size_t maxlen = sizeof(sockaddr->sun_path);
	uid_t uid = getuid();
	struct passwd *pw = getpwuid(uid);

	for (unsigned int i = 0; i < countof(socket_dirs); i++) {
		struct Dir *dir = &socket_dirs[i];
		if (dir->mode == SKIP)
			continue;
		char *selected = NULL;
		bool ishome = false;
		if (dir->mode == PERSONAL_ENV || dir->mode == COMMON_ENV) {
			selected = getenv(dir->reference);
			ishome = !strcmp(dir->reference, "HOME");
			if (ishome && (!selected || !selected[0]) && pw)
				selected = pw->pw_dir;
		}
		if (dir->mode == PATH)
			selected = dir->reference;
		if (!selected || !selected[0])
			continue;
		if (dir->mode != PATH)
			if (!xsnprintf(sockaddr->sun_path, maxlen, "%s/%s%s/", selected, ishome ? "." : "", server.name))
				continue;
		mode_t mask = umask(0);
		int personal = PERSONAL_ENV == dir->mode;
		int r = mkdir(sockaddr->sun_path, personal ? S_IRWXU : S_IRWXU|S_IRWXG|S_IRWXO|S_ISVTX);
		umask(mask);
		if (r != 0 && errno != EEXIST)
			continue;
		errno = 0;
		struct stat sb;
		if (lstat(sockaddr->sun_path, &sb) != 0)
			continue;
		if (!S_ISDIR(sb.st_mode)) {
			errno = ENOTDIR;
			continue;
		}

		size_t dirlen = strlen(sockaddr->sun_path);
		if (!personal) {
			// create subdirectory only accessible to user
			if (pw && !xsnprintf(sockaddr->sun_path+dirlen, maxlen-dirlen, "%s/", pw->pw_name))
				continue;
			if (!pw && !xsnprintf(sockaddr->sun_path+dirlen, maxlen-dirlen, "%d/", uid))
				continue;
			if (mkdir(sockaddr->sun_path, S_IRWXU) != 0 && errno != EEXIST)
				continue;
			if (lstat(sockaddr->sun_path, &sb) != 0)
				continue;
			if (!S_ISDIR(sb.st_mode)) {
				errno = ENOTDIR;
				continue;
			}
			dirlen = strlen(sockaddr->sun_path);
		}

		if (sb.st_uid != uid || sb.st_mode & (S_IRWXG|S_IRWXO)) {
			errno = EACCES;
			continue;
		}

		if (!xsnprintf(sockaddr->sun_path+dirlen, maxlen-dirlen, ".adduco-%d", getpid()))
			continue;

		socklen_t socklen = offsetof(struct sockaddr_un, sun_path) + strlen(sockaddr->sun_path) + 1;
		if (bind(socketfd, (struct sockaddr*)sockaddr, socklen) == -1)
			continue;
		unlink(sockaddr->sun_path);
		sockaddr->sun_path[dirlen] = '\0';

		strncpy(cache, sockaddr->sun_path, sizeof(cache));
		strncpy(path, cache, path_max_len);
		close(socketfd);
		return true;
	}

	close(socketfd);
	return false;
}

static bool set_socket_name(struct sockaddr_un *sockaddr, const char *name) {
	const size_t maxlen = sizeof(sockaddr->sun_path);
	const char *session_name = NULL;
	char buf[maxlen];

	if (name[0] == '/') {
		if (strlen(name) >= maxlen) {
			errno = ENAMETOOLONG;
			return false;
		}
		strncpy(sockaddr->sun_path, name, maxlen);
	} else if (name[0] == '.' && (name[1] == '.' || name[1] == '/')) {
		char *cwd = getcwd(buf, sizeof buf);
		if (!cwd)
			return false;
		if (!xsnprintf(sockaddr->sun_path, maxlen, "%s/%s", cwd, name))
			return false;
	} else {
		if (!create_socket_dir(sockaddr->sun_path, sizeof(sockaddr->sun_path)))
			return false;
		if (strlen(sockaddr->sun_path) + strlen(name) + strlen(server.host) >= maxlen) {
			errno = ENAMETOOLONG;
			return false;
		}
		session_name = name;
		strncat(sockaddr->sun_path, name, maxlen - strlen(sockaddr->sun_path) - 1);
		strncat(sockaddr->sun_path, server.host, maxlen - strlen(sockaddr->sun_path) - 1);
	}

	if (!session_name) {
		strncpy(buf, sockaddr->sun_path, sizeof buf);
		session_name = basename(buf);
	}
	setenv("ADDUCO_SESSION", session_name, 1);
	setenv("ADDUCO_SOCKET", sockaddr->sun_path, 1);

	return true;
}

static bool create_session(const char *name, char * const argv[]) {
	// this uses the well known double fork strategy as described in section 1.7 of
	//
	//  http://www.faqs.org/faqs/unix-faq/programmer/faq/
	//
	// pipes are used for synchronization and error reporting i.e. the child sets
	// the close on exec flag before calling execvp(3) the parent blocks on a read(2)
	// in case of failure the error message is written to the pipe, success is
	// indicated by EOF on the pipe.
	//
	int client_pipe[2], server_pipe[2];
	pid_t pid;
	char errormsg[255];
	struct sigaction sa;

	if (session_exists(name)) {
		errno = EADDRINUSE;
		return false;
	}

	if (pipe(client_pipe) == -1)
		return false;
	if ((server.socket = server_create_socket(name)) == -1)
		return false;

	switch ((pid = fork())) {
	case 0: /* child process */
		setsid();
		close(client_pipe[0]);
		switch ((pid = fork())) {
		case 0: /* child process */
			if (pipe(server_pipe) == -1) {
				snprintf(errormsg, sizeof(errormsg), "server-pipe: %s\n", strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				_exit(EXIT_FAILURE);
			}
			sa.sa_flags = 0;
			sigemptyset(&sa.sa_mask);
			sa.sa_handler = server_pty_died_handler;
			sigaction(SIGCHLD, &sa, NULL);
			switch (server.pid = forkpty(&server.pty, NULL, options.has_term ? &server.term : NULL, &server.winsize)) {
			case 0: /* child = user application process */
				close(server.socket);
				close(server_pipe[0]);
				if (fcntl(client_pipe[1], F_SETFD, FD_CLOEXEC) == 0 &&
				    fcntl(server_pipe[1], F_SETFD, FD_CLOEXEC) == 0)
					execvp(argv[0], argv);
				snprintf(errormsg, sizeof(errormsg), "server-execvp: %s: %s\n",
						 argv[0], strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				write_all(server_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				close(server_pipe[1]);
				_exit(EXIT_FAILURE);
				break;
			case -1: /* forkpty failed */
				snprintf(errormsg, sizeof(errormsg), "server-forkpty: %s\n", strerror(errno));
				write_all(client_pipe[1], errormsg, strlen(errormsg));
				close(client_pipe[1]);
				close(server_pipe[0]);
				close(server_pipe[1]);
				_exit(EXIT_FAILURE);
				break;
			default: /* parent = server process */
				sa.sa_handler = server_sigterm_handler;
				sigaction(SIGTERM, &sa, NULL);
				sigaction(SIGINT, &sa, NULL);
				sa.sa_handler = server_sigusr1_handler;
				sigaction(SIGUSR1, &sa, NULL);
				sa.sa_handler = SIG_IGN;
				sigaction(SIGPIPE, &sa, NULL);
				sigaction(SIGHUP, &sa, NULL);
				if (chdir("/") == -1)
					_exit(EXIT_FAILURE);
			#ifdef NDEBUG
				int fd = open("/dev/null", O_RDWR);
				if (fd != -1) {
					dup2(fd, STDIN_FILENO);
					dup2(fd, STDOUT_FILENO);
					dup2(fd, STDERR_FILENO);
					close(fd);
				}
			#endif /* NDEBUG */
				close(client_pipe[1]);
				close(server_pipe[1]);
				if (read_all(server_pipe[0], errormsg, sizeof(errormsg)) > 0)
					_exit(EXIT_FAILURE);
				close(server_pipe[0]);
				server_mainloop();
				break;
			}
			break;
		case -1: /* fork failed */
			snprintf(errormsg, sizeof(errormsg), "server-fork: %s\n", strerror(errno));
			write_all(client_pipe[1], errormsg, strlen(errormsg));
			close(client_pipe[1]);
			_exit(EXIT_FAILURE);
			break;
		default: /* parent = intermediate process */
			close(client_pipe[1]);
			_exit(EXIT_SUCCESS);
			break;
		}
		break;
	case -1: /* fork failed */
		close(client_pipe[0]);
		close(client_pipe[1]);
		return false;
	default: /* parent = client process */
		close(client_pipe[1]);
		while (waitpid(pid, NULL, 0) == -1 && errno == EINTR);
		ssize_t len = read_all(client_pipe[0], errormsg, sizeof(errormsg));
		if (len > 0) {
			write_all(STDERR_FILENO, errormsg, len);
			unlink(sockaddr.sun_path);
			exit(EXIT_FAILURE);
		}
		close(client_pipe[0]);
	}
	return true;
}

static bool attach_session(const char *name, const bool terminate) {
	if (server.socket > 0)
		close(server.socket);
	if ((server.socket = session_connect(name)) == -1)
		return false;
	if (server_set_socket_non_blocking(server.socket) == -1)
		return false;

	struct sigaction sa;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = client_sigwinch_handler;
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	client_setup_terminal();
	int status = client_mainloop();
	client_restore_terminal();
	if (status == -1) {
		info(INFO, "detached");
	} else if (status == -EIO) {
		info(INFO, "exited due to I/O errors");
	} else {
		info(INFO, "session terminated with exit status %d", status);
		if (terminate)
			exit(status);
	}

	return terminate;
}

static bool rename_session(const char *name, const char *newname) {
	bool result = false;

	if (server.socket > 0)
		close(server.socket);
	if ((server.socket = session_connect(name)) == -1)
		return false;

	if (!session_get_connection_ack())
		goto end;

	// ask the session server to rename the communication socket
	Packet pkt;
	pkt.type = MSG_RENAME;
	pkt.len = strlen(newname) + 1;
	if (pkt.len >= sizeof(pkt.u.msg))
		pkt.len = sizeof(pkt.u.msg) - 1;
	strncpy(pkt.u.msg, newname, sizeof(pkt.u.msg) - 1);
	pkt.u.msg[sizeof(pkt.u.msg) - 1] = (char)0;
	if (!send_packet(server.socket, &pkt))
		goto end;

	// wait for the server to acknowledge the rename
	pkt.type = MSG_PID;
	if (!client_recv_packet(&pkt) || pkt.type != MSG_RENAME)
		goto end;

	result = (pkt.u.i == 1);

end:
	close(server.socket);
	return result;
}

static int session_filter(const struct dirent *d) {
	return strstr(d->d_name, server.host) != NULL;
}

static int session_comparator(const struct dirent **a, const struct dirent **b) {
	struct stat sa, sb;
	if (stat((*a)->d_name, &sa) != 0)
		return -1;
	if (stat((*b)->d_name, &sb) != 0)
		return 1;
	return sa.st_atime < sb.st_atime ? -1 : 1;
}

struct session_iterator {
	struct dirent **namelist;
	int count;
	int current;
	pid_t pid;
	struct stat sb;
	char info;
};

// continue to iterate over sessions, stop when it returns false
static int iterate_over_sessions(struct session_iterator *result) {
	result->info = 'E'; /* iteration error */
	if (!result->namelist) {
		result->count = -1;
		result->current = -1;
		if (!create_socket_dir(sockaddr.sun_path, sizeof(sockaddr.sun_path)))
			return 0;
		if (chdir(sockaddr.sun_path) == -1)
			return 0;
		result->count = scandir(sockaddr.sun_path, &result->namelist, session_filter, session_comparator);
		if (result->count < 0)
			return 0;
	}
	result->info = ' '; /* background session */
	result->current += 1;
	if (result->count > 0 && result->current < result->count) {
		struct dirent *item = result->namelist[result->current];
		if (stat(item->d_name, &result->sb) == 0 && S_ISSOCK(result->sb.st_mode)) {
			char *local = strstr(item->d_name, server.host);
			if (local) {
				*local = '\0'; /* truncate hostname if we are local */
				if (!(result->pid = session_exists(item->d_name)))
					return iterate_over_sessions(result); /* try next item */
				if (result->sb.st_mode & S_IXUSR)
					result->info = '*'; /* session with connected client */
				else if (result->sb.st_mode & S_IXGRP)
					result->info = '+'; /* dead session waiting for a client to collect the exit status */
			}
		}
	}
	if (result->current >= result->count){
		for (int n = 0; n < result->count; n += 1)
			free(result->namelist[n]);
		free(result->namelist);
		result->namelist = NULL;
		result->count = 0;
	}
	return result->namelist != NULL;
}

static int print_session_list(void) {
	struct session_iterator iter = {0};
	printf("Active sessions (on host %s)\n", server.host+1);
  while(iterate_over_sessions(&iter)) {
		char buf[255];
		strftime(buf, sizeof(buf), "%a%t %F %T", localtime(&iter.sb.st_mtime));
		printf("%c %s\t%jd\t%s\n", iter.info, buf, (intmax_t)iter.pid, iter.namelist[iter.current]->d_name);
  }
  return iter.info == 'E'; /* E -> error while iterating */
}

static int signal_to_session(int signal, const char* name) {
	if (server.socket > 0)
		close(server.socket);
	if ((server.socket = session_connect(name)) == -1)
		return -1;

	Packet pkt = {
		.type = MSG_SIGNAL,
		.u.i = signal,
		.len = sizeof(pkt.u.i),
	};
	if (!client_send_packet(&pkt)) {
		close(server.socket);
		return -1;
	}
	int result = session_get_connection_ack() ? 0 : -1; 
	close(server.socket);
	return result;
}

static char ** get_default_command(char* command){
	// using shell wrapper mostly to parse space-saparated arguments, however it
	// adds lot of flexibility when the command is not passed on the command line.
	static char *default_command = NULL;
	static char *command_line[] = {NULL, "-c", NULL, NULL};
	if (default_command == NULL) {
		char *from_environ;
		from_environ = getenv(ADDUCO_SHELL_ENV);
		if (!from_environ || *from_environ == '\0') {
			command_line[0] = ADDUCO_SHELL;
		} else {
			command_line[0] = from_environ;
		}
		from_environ = getenv(ADDUCO_CMD_ENV);
		if (!from_environ || *from_environ == '\0') {
			default_command = ADDUCO_CMD;
		} else {
			default_command = from_environ;
		}
	}
	if (command) {
		command_line[2] = command;
	} else {
		command_line[2] = default_command;
  }
	return command_line;
}

// --------------------------------------------------------------------------------
// TUI

enum {
	KEY_NONE  = 0,
	KEY_UP,
	KEY_DOWN,
	KEY_ENTER,
	KEY_QUIT,
	KEY_KILL,
	KEY_CREATE,
	KEY_RENAME,
	KEY_ATTACH_RO,
};

struct tui_session {
	char *name;
	char *info;
};

static struct termios orig_term;
static int raw_active = 0;

// restore the terminal to its original settings (idempotent)
static void tui_restore_term(void) {
	if (raw_active) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
		raw_active = 0;
	}
	// make sure the cursor is visible again
	fputs("\033[?25h", stdout);
	fflush(stdout);
}

// switch the terminal into raw mode so we can read single key presses
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

// read a single byte from stdin, waiting at most timeout_ms (or forever if < 0).
// returns the byte or -1 on timeout / error / EOF.
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

// read and decode the next key press
static int tui_read_key(void) {

	int c = tui_read_byte(-1);
	if (c < 0)
		return KEY_QUIT;

	switch (c) {
	default: return KEY_NONE;
	case '\n': return KEY_ENTER;
	case '\r': return KEY_ENTER;
	case 'q': return KEY_QUIT;
	case 'd': return KEY_KILL;
	case 'c': return KEY_CREATE;
	case 'm': return KEY_RENAME;
	case 'r': return KEY_ATTACH_RO;
	case 'j': return KEY_DOWN;
	case 'k': return KEY_UP;
	case 27:

		// an escape sequence (e.g. arrow keys) starts with ESC [ <letter>
		c = tui_read_byte(50);
		if (c < 0)
			return KEY_QUIT;   /* lone ESC -> quit */
		if (c != '[')
			return KEY_NONE;

		c = tui_read_byte(50);
		if (c < 0)
			return KEY_NONE;

		switch (c) {
		default: return KEY_NONE;
		case 'A': return KEY_UP;
		case 'B': return KEY_DOWN;
		}
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

// redraw the whole menu, keeping the selection visible (simple viewport)
static void tui_draw(struct tui_session *names, int count, int sel, int *top, const char *msg) {

	int avail = tui_clear_for_lines(count + 3); /* items + title + help + status/empty line */

	if (avail < 1)
		avail = 1;

	if (sel < *top)
		*top = sel;
	if (sel >= *top + avail)
		*top = sel - avail + 1;
	if (*top < 0)
		*top = 0;

	char path[256] = {0};
	create_socket_dir(path, sizeof(path));
	fprintf(stdout, "\033[0mSession store: %s - ", path);
	if (count == 0)
		fprintf(stdout, "No active sessions.\r\n - ---\r\n");
	else {
		fprintf(stdout, "%d session(s):\r\n", count);

		for (int i = 0; i < avail; i++) {
			int idx = *top + i;
			if (idx >= count)
				break;
			fputs(idx == sel ? "\033[7m" : "\033[0m", stdout);
			fputs(idx == sel ? " >" : "  ", stdout);
			fprintf(stdout, "%s", names[idx].info);
			fputs("\033[0m\r\n", stdout);
		}
	}

	fputs("[Arrows move] [d kill] [c create] [ENTER attach] [m rename] [r attach read-only] [ctrl-\\ detach] [q quit]\r\n", stdout);

	if (msg && *msg)
		fprintf(stdout, "Info: %s", msg);

	fflush(stdout);
}

static void session_list_free(struct tui_session *names, int count) {
	if (!names)
		return;
	for (int i = 0; i < count; i++){
		free(names[i].name);
		free(names[i].info);
  }
	free(names);
}

// Collect the names of all active sessions into *names Returns the number of
// sessions or -1 on error.
static void tui_session_list(struct tui_session **names, int *count) {
	if (*count > 0){
		session_list_free(*names, *count);
	}
	*count = 0;
	struct tui_session *list = NULL;
	struct session_iterator iter = {0};
	while(iterate_over_sessions(&iter)){
		struct tui_session *tmp = realloc(list, (*count + 1) * sizeof *tmp);
		if (tmp) {
			char fmt[] = " %s [%c] %d";
			int needed = snprintf(NULL, 0, fmt, iter.namelist[iter.current]->d_name, iter.info, iter.pid);
			char * info = calloc(needed+1, 1);
			snprintf(info, needed+1, fmt, iter.namelist[iter.current]->d_name, iter.info, iter.pid);
			list = tmp;
			list[(*count)].name = strdup(iter.namelist[iter.current]->d_name);
			list[(*count)].info = info;
			(*count)++;
		}
	}
	*names = list;
}

// Keep a stable ordering of sessions across refreshes.
static void tui_apply_order(struct tui_session *names, int count,
                            char ***order, int *order_count) {
	// No sessions left: forget the saved ordering entirely.
	if (count <= 0) {
		for (int i = 0; i < *order_count; i++)
			free((*order)[i]);
		free(*order);
		*order = NULL;
		*order_count = 0;
		return;
	}

	int *placed = calloc(count, sizeof(int));   /* 1 once names[i] is placed */
	int *pos    = calloc(count, sizeof(int));   /* new index for names[i]    */
	char **new_order = NULL;
	int new_count = 0;

	// Pass 1: keep the relative order of known sessions still present.
	for (int o = 0; o < *order_count; o++) {
		for (int i = 0; i < count; i++) {
			if (!placed[i] && strcmp(names[i].name, (*order)[o]) == 0) {
				pos[i] = new_count++;
				placed[i] = 1;
				new_order = realloc(new_order, new_count * sizeof(char *));
				new_order[new_count - 1] = strdup((*order)[o]);
				break;
			}
		}
	}
	// Pass 2: append sessions never seen before, at the end.
	for (int i = 0; i < count; i++) {
		if (!placed[i]) {
			pos[i] = new_count++;
			placed[i] = 1;
			new_order = realloc(new_order, new_count * sizeof(char *));
			new_order[new_count - 1] = strdup(names[i].name);
		}
	}

	// Physically reorder the names array (move the struct values).
	struct tui_session *tmp = calloc(count, sizeof(struct tui_session));
	for (int i = 0; i < count; i++)
		tmp[pos[i]] = names[i];
	memcpy(names, tmp, count * sizeof(struct tui_session));
	free(tmp);

	// Replace the persistent ordering with the new one.
	for (int o = 0; o < *order_count; o++)
		free((*order)[o]);
	free(*order);
	*order = new_order;
	*order_count = new_count;

	free(placed);
	free(pos);
}

// Move a session to the front of the persistent session ordering.
static void tui_bump_to_front(char ***order, int *order_count, const char *name) {
	if (!name || !order || !order_count)
		return;
	int idx = -1;
	for (int i = 0; i < *order_count; i++) {
		if (strcmp((*order)[i], name) == 0) {
			idx = i;
			break;
		}
	}
	if (idx == 0)             /* already at the front */
		return;
	if (idx > 0) {            /* known session: shift it to the front */
		char *bumped = (*order)[idx];
		for (int i = idx; i > 0; i--)
			(*order)[i] = (*order)[i - 1];
		(*order)[0] = bumped;
		return;
	}
	/* unknown session: prepend a brand new entry at the front */
	char **new_order = realloc(*order, (*order_count + 1) * sizeof(char *));
	if (!new_order)
		return;
	for (int i = *order_count; i > 0; i--)
		new_order[i] = new_order[i - 1];
	new_order[0] = strdup(name);
	if (!new_order[0]) {     /* strdup failed: keep the old ordering */
		free(new_order);
		return;
	}
	*order = new_order;
	(*order_count)++;
}

// Suspend the interactive session list, attach to the named session
static void tui_attach_session(const char *name, bool readonly,
				char ***order, int *order_count) {
	tui_restore_term();
	if (readonly)
		client.flags |= CLIENT_READONLY;
	attach_session(name, false);
	if (readonly)
		client.flags &= ~CLIENT_READONLY;
	if (!readonly)
		tui_bump_to_front(order, order_count, name);
	tui_enter_raw();
}

// Ask the user to confirm killing the selected session. Returns true if confirmed.
static bool tui_confirm_kill(const char *name) {
	tui_clear_for_lines(2); /* prompt + empty.status line */
	fputs("\033[1mKill session\033[0m '", stdout);
	fputs(name, stdout);
	fputs("' ? [y/N]\r\n", stdout);
	fflush(stdout);
	int c = tui_read_byte(-1);
	return c == 'y' || c == 'Y';
}

// Read a line of text from the user. Returns a malloc'd, NUL-terminated string
// on Enter (which may be empty), or NULL if the user pressed ESC to cancel.
// The terminal is in raw mode, so characters are echoed manually.
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

static void tui_random_name(char *buf, size_t sz) {
	static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	static bool seeded = false;
	if (!seeded) {
		srand((unsigned)(time(NULL) ^ getpid()));
		seeded = true;
	}
	const size_t maxlen = sz - 1;
	do {
		size_t i;
		for (i = 0; i < maxlen; i++)
			buf[i] = charset[(unsigned)rand() % (sizeof(charset) - 1)];
		buf[i] = '\0';
	} while (session_exists(buf));
}

// Prompt the user for a command to run and a session name, then create a new
// session.
static char *tui_create_session(const char **msg) {
	char **argv = get_default_command(NULL);
	char *defcmd = argv[2];

	char cmdprompt[512];
	snprintf(cmdprompt, sizeof(cmdprompt), "Command to run [%s] (ESC to cancel): ", defcmd);

	char *cmd = tui_read_line(cmdprompt);
	if (!cmd) {
		*msg = "Create cancelled.";
		return NULL;
	}

	// generate a random session name up front, then offer it as the default
	char random_name[8];
	tui_random_name(random_name, sizeof(random_name));

	char nameprompt[sizeof(random_name) + 64];
	snprintf(nameprompt, sizeof(nameprompt),
	         "Session name [%s] (ESC to cancel): ", random_name);

	char *name = tui_read_line(nameprompt);
	if (!name) {
		free(cmd);
		*msg = "Create cancelled.";
		return NULL;
	}

	// empty input -> fall back to the generated random name
	if (name[0] == '\0') {
		free(name);
		name = strdup(random_name);
		if (!name) {
			free(cmd);
			*msg = "Out of memory.";
			return NULL;
		}
	}

	if (session_exists(name)) {
		free(cmd);
		free(name);
		*msg = "Session already exists.";
		return NULL;
	}

	if (cmd[0] != '\0') {
		argv = get_default_command(cmd);
	}

	if (!create_session(name, argv)) {
		free(name);
		name = NULL;
		*msg = "Could not create session.";
	}
	free(cmd);

	// ownership of the session name is transferred to the caller
	return name;
}

// Prompt for a new name and rename the selected session. Pressing ESC at the
// prompt cancels and returns to the session list (NULL). On success the
// malloc'd new session name is returned and its ownership transferred to the
// caller, so the selection can be kept on the renamed session. If the new name
// is empty or already in use, NULL is returned.
static char *tui_rename_session(struct tui_session *names, int count, int sel, int *top) {
	char *name = names[sel].name;
	char *newname = tui_read_line("New session name (ESC to cancel): ");
	if (!newname)
		return NULL;
	if (newname[0] == '\0') {
		free(newname);
		return NULL;
	}
	if (session_exists(newname)) {
		free(newname);
		return NULL;
	}
	if (rename_session(name, newname))
		return newname;   /* ownership transferred to caller */
	free(newname);
	return NULL;
}

// Resolve the index of the session named 'sel' within 'names', or return -1
// if no session with that name is currently present.
static int tui_find_index(struct tui_session *names, int count, const char *sel) {
	if (!sel)
		return -1;
	for (int i = 0; i < count; i++)
		if (strcmp(names[i].name, sel) == 0)
			return i;
	return -1;
}

void tui_main(void) {
	struct tui_session *names = NULL;
	int count = 0;
	char *sel_name = NULL;   /* canonical selection: name of the chosen session */
	int sel;                 /* derived index into names[], resolved each refresh */
	int top = 0;
	char **order = NULL;       /* persistent session ordering */
	int order_count = 0;
	const char *msg = NULL;

	tui_enter_raw();
	atexit(tui_restore_term);

	for (;;) {
		tui_session_list(&names, &count);
		if (count < 0)
			count = 0;
		// keep a stable ordering across refreshes
		tui_apply_order(names, count, &order, &order_count);

		// Resolve the selection by name. If the previously selected
		// session is gone (or none was selected yet), fall back to the
		// first session of the freshly built list.
		sel = tui_find_index(names, count, sel_name);
		if (sel < 0) {
			sel = 0;
			free(sel_name);
			sel_name = count > 0 ? strdup(names[0].name) : NULL;
		}

		tui_draw(names, count, sel, &top, msg);
		msg = NULL;

		int k = tui_read_key();
		if (k == KEY_QUIT) {
			break;
		} else if (k == KEY_UP) {
			if (sel > 0) {
				sel--;
				free(sel_name);
				sel_name = strdup(names[sel].name);
			}
		} else if (k == KEY_DOWN) {
			if (sel < count - 1) {
				sel++;
				free(sel_name);
				sel_name = strdup(names[sel].name);
			}
		} else if (k == KEY_ENTER) {
			if (count > 0 && sel < count) {
				char *name = strdup(names[sel].name);
				session_list_free(names, count);
				names = NULL;
				count = 0;
				tui_attach_session(name, false, &order, &order_count);
				free(name);
			}
		} else if (k == KEY_ATTACH_RO) {
			if (count > 0 && sel < count) {
				char *name = strdup(names[sel].name);
				session_list_free(names, count);
				names = NULL;
				count = 0;
				tui_attach_session(name, true, &order, &order_count);
				free(name);
			}
		} else if (k == KEY_KILL) {
			if (count > 0 && sel < count) {
				const char *name = names[sel].name;
				if (tui_confirm_kill(name)) {
					if (signal_to_session(SIGTERM, name))
						msg = "Could not kill session.";
					else
						msg = "Session killed.";
				} else {
					msg = "Kill aborted.";
				}
				// keep sel_name: it will not be found after the
				// refresh, so the first session gets selected
			}
		} else if (k == KEY_CREATE) {
			char *created = tui_create_session(&msg);
			if (created) {
				session_list_free(names, count);
				names = NULL;
				count = 0;
				free(sel_name);
				sel_name = strdup(created);
				tui_attach_session(created, false, &order, &order_count);
				free(created);
			}
		} else if (k == KEY_RENAME) {
			if (count > 0 && sel < count) {
				char *newname = tui_rename_session(names, count, sel, &top);
				if (newname) {
					free(sel_name);
					sel_name = newname;   /* stay on the renamed session */
				}
			}
		}
	}

	tui_restore_term();
	session_list_free(names, count);
	free(sel_name);
	for (int i = 0; i < order_count; i++)
		free(order[i]);
	free(order);
}

// --------------------------------------------------------------------------------
// CLI parsing

int main(int argc, char *argv[]) {
	int opt;
	bool force = false;
	char **cmd = NULL, action = 'i'; /* interactive mode by default */
	const char *rename_target = NULL;

	server.name = basename(argv[0]);
	gethostname(server.host+1, sizeof(server.host) - 1);

	while ((opt = getopt(argc, argv, "aAclne:fpqrkm:hs")) != -1) {
		switch (opt) {
		case 'a':
		case 'A':
		case 'c':
		case 'n':
		case 'k':
		case 'h':
		case 's':
			action = opt;
			break;
		case 'm':
			action = opt;
			rename_target = optarg;
      break;
		case 'e':
			if (!optarg)
				wrong_usage();
			if (optarg[0] == '^' && optarg[1])
				optarg[0] = CTRL(optarg[1]);
			KEY_DETACH = optarg[0];
			break;
		case 'f':
			force = true;
			break;
		case 'p':
			options.passthrough = true;
			break;
		case 'q':
			options.quiet = true;
			break;
		case 'r':
			client.flags |= CLIENT_READONLY;
			break;
		case 'l':
			client.flags |= CLIENT_LOWPRIORITY;
			break;
		default:
			wrong_usage();
		}
	}

	// collect the session name if trailing args
	if (optind < argc){
		strncpy(server.session_name, argv[optind], sizeof(server.session_name));
		server.session_name[sizeof(server.session_name)-1] = '\0';
  }

	// if yet more trailing arguments, they must be the command
	if (optind + 1 < argc)
		cmd = &argv[optind + 1];
	else
		cmd = get_default_command(NULL);

	if (server.session_name[0] != '\0' && !isatty(STDIN_FILENO) && action != 'i')
		options.passthrough = true;

	if (options.passthrough && action == 'i')
		wrong_usage();

	if (options.passthrough) {
			options.quiet = true;
			client.flags |= CLIENT_LOWPRIORITY;
	}

	if (action != 'i' && action != 's' && action != 'h' && server.session_name[0] == '\0')
		wrong_usage();

	if (!options.passthrough && tcgetattr(STDIN_FILENO, &orig_term) != -1) {
		server.term = orig_term;
		options.has_term = true;
	}

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &server.winsize) == -1) {
		server.winsize.ws_col = 80;
		server.winsize.ws_row = 25;
	}

	server.read_pty = (action == 'n');

	redo:
	switch (action) {
	case 'n':
	case 'c':
		if (force) {
			if (session_alive(server.session_name)) {
				info(INFO, "session exists and has not yet terminated");
				return 1;
			}
			if (session_exists(server.session_name))
				attach_session(server.session_name, false);
		}
		if (!create_session(server.session_name, cmd))
			die("create-session");
		if (action == 'n')
			break;
		// fall through
	case 'a':
		if (!attach_session(server.session_name, true))
			die("attach-session");
		break;
	case 'A':
		if (session_alive(server.session_name)) {
			if (!attach_session(server.session_name, true))
				die("attach-session");
		} else if (!attach_session(server.session_name, !force)) {
			force = false;
			action = 'c';
			goto redo;
		}
		break;
	case 'k':
		if (signal_to_session(SIGTERM, server.session_name))
			die("kill-session: kill");
		if (!options.quiet)
			info(INFO, "session killed");
		break;
	case 'm':
		if (!rename_session(server.session_name, rename_target)) {
			die("can not rename session");
		} else if (!options.quiet) {
			info(INFO, "session renamed to %s", rename_target);
		}
		break;
	case 'i':
		tui_main();
		break;
	case 'h':
		print_help();
		break;
	case 's':
		if (print_session_list() != 0)
			die("list-session");
		break;
	}

	return 0;
}


