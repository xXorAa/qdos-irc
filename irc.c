#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <curses.h>
#include <unistd.h>
#ifdef QDOS
#include <sys/bsdtypes.h>
#include <qdos.h>
#endif
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <locale.h>
#include <wchar.h>

#undef CTRL
#define CTRL(x)  (x & 037)

#define SCROLL   15
#define INDENT   23
#define DATEFMT  "%H:%M"
#define PFMT     "  %-12s < %s"
#define PFMTHIGH "> %-12s < %s"
#define SRV      "chat.qlforum.co.uk"
#define PORT     "6667"

enum {
	ChanLen = 64,
	LineLen = 512,
	MaxChans = 16,
	BufSz = 2048,
	LogSz = 4096,
	MaxRecons = 10, /* -1 for infinitely many */
	UtfSz = 4,
	RuneInvalid = 0xFFFD,
};

typedef wchar_t Rune;

static struct {
	int x;
	int y;
	WINDOW *sw, *mw, *iw;
} scr;

static struct Chan {
	char name[ChanLen];
	char *buf, *eol;
	int n;     /* Scroll offset. */
	size_t sz; /* Size of buf. */
	char high; /* Nick highlight. */
	char new;  /* New message. */
	char join; /* Channel was 'j'-oined. */
} chl[MaxChans];

/* QDOS Stuff */
WINDOWDEF_t _condetails = {
	2, /* border colour (red) */
	0, /* border width */
	0, /* paper (black) */
	7, /* ink (white) */
	512, /* width (pixel) */
	256, /* height (pixels) */
	0, /* x origin */
	0 /* y origin */
};

char _prog_name[] = "qdos-irc";
long _stack = 16L * 1024L;

static int ssl;
static struct {
	int fd;
} srv;
static char nick[64];
static int quit;
static int nch, ch; /* Current number of channels, and current channel. */
static char outb[BufSz], *outp = outb; /* Output buffer. */
static FILE *logfp;

static unsigned char utfbyte[UtfSz + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static unsigned char utfmask[UtfSz + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};

static void scmd(char *, char *, char *, char *);
static void tdrawbar(void);
static void tredraw(void);
static void treset(void);

static void
panic(const char *m)
{
	treset();
	fprintf(stderr, "Panic: %s\n", m);
	exit(1);
}

static void
sndf(const char *fmt, ...)
{
	va_list vl;
	size_t n, l = BufSz - (outp - outb);

	if (l < 2)
		return;
	va_start(vl, fmt);
	n = vsprintf(outp, fmt, vl);
	va_end(vl);
	outp += n > l - 2 ? l - 2 : n;
	*outp++ = '\r';
	*outp++ = '\n';
}

static int
srd(void)
{
	static char l[BufSz], *p = l;
	char *s, *usr, *cmd, *par, *data;
	int rd;

	if (p - l >= BufSz)
		p = l; /* Input buffer overflow, there should something better to do. */
	rd = read(srv.fd, p, BufSz - (p - l));
	if (rd <= 0)
		return 0;
	p += rd;
	for (;;) { /* Cycle on all received lines. */
		if (!(s = memchr(l, '\n', p - l)))
			return 1;
		if (s > l && s[-1] == '\r')
			s[-1] = 0;
		*s++ = 0;
		if (*l == ':') {
			if (!(cmd = strchr(l, ' ')))
				goto lskip;
			*cmd++ = 0;
			usr = l + 1;
		} else {
			usr = 0;
			cmd = l;
		}
		if (!(par = strchr(cmd, ' ')))
			goto lskip;
		*par++ = 0;
		if ((data = strchr(par, ':')))
			*data++ = 0;
		scmd(usr, cmd, par, data);
	lskip:
		memmove(l, s, p - s);
		p -= s - l;
	}
}

static void
sinit(const char *key, const char *nick, const char *user)
{
	if (key)
		sndf("PASS %s", key);
	sndf("NICK %s", nick);
	sndf("USER %s 8 * :%s", user, user);
	sndf("MODE %s +i", nick);
}

static char *
dial(const char *host, const char *service)
{
    struct hostent *server;
    struct sockaddr_in conn;
	int fd = -1, e;
    int port;

    server = gethostbyname(host);
    port = atoi(service);

    conn.sin_family = AF_INET;
    conn.sin_port = htons(port);
    conn.sin_addr.s_addr = *(long *)(server->h_addr_list[0]);

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return "Cannon open socket.";
	if (connect(fd, (struct sockaddr *)&conn, sizeof(conn)) == -1) {
		close(fd);
        return "Cannot connect to host.";
	}
	srv.fd = fd;
	return 0;
}

static void
hangup(void)
{
	if (srv.fd) {
		close(srv.fd);
		srv.fd = 0;
	}
}

static int
chfind(const char *name)
{
	int i;

	assert(name);
	for (i = nch - 1; i > 0; i--)
		if (!strcmp(chl[i].name, name))
			break;
	return i;
}

static int
chadd(const char *name, int joined)
{
	int n;

	if (nch >= MaxChans || strlen(name) >= ChanLen)
		return -1;
	if ((n = chfind(name)) > 0)
		return n;
	strcpy(chl[nch].name, name);
	chl[nch].sz = LogSz;
	chl[nch].buf = malloc(LogSz);
	if (!chl[nch].buf)
		panic("Out of memory.");
	chl[nch].eol = chl[nch].buf;
	chl[nch].n = 0;
	chl[nch].join = joined;
	if (joined)
		ch = nch;
	nch++;
	tdrawbar();
	return nch;
}

static int
chdel(char *name)
{
	int n;

	if (!(n = chfind(name)))
		return 0;
	nch--;
	free(chl[n].buf);
	memmove(&chl[n], &chl[n + 1], (nch - n) * sizeof(struct Chan));
	ch = nch - 1;
	tdrawbar();
	return 1;
}

static char *
pushl(char *p, char *e)
{
	int x, cl;
	char *w;
	Rune u[2];

	u[1] = 0;
	if ((w = memchr(p, '\n', e - p)))
		e = w + 1;
	w = p;
	x = 0;
	for (;;) {
		if (x >= scr.x) {
			waddch(scr.mw, '\n');
			for (x = 0; x < INDENT; x++)
				waddch(scr.mw, ' ');
			if (*w == ' ')
				w++;
			x += p - w;
		}
		if (p >= e || *p == ' ' || p - w + INDENT >= scr.x - 1) {
			while (w < p) {
				waddch(scr.mw, *w++);
			}
			if (p >= e)
				return e;
		}
		p++;
		x++;
	}
}

static void
pushf(int cn, const char *fmt, ...)
{
	struct Chan *const c = &chl[cn];
	size_t n, blen = c->eol - c->buf;
	va_list vl;
	time_t t;
	char *s;
	struct tm *tm, *gmtm;

	if (blen + LineLen >= c->sz) {
		c->sz *= 2;
		c->buf = realloc(c->buf, c->sz);
		if (!c->buf)
			panic("Out of memory.");
		c->eol = c->buf + blen;
	}
	t = time(0);
	if (!(tm = localtime(&t)))
		panic("Localtime failed.");
	n = strftime(c->eol, LineLen, DATEFMT, tm);
	if (!(gmtm = gmtime(&t)))
		panic("Gmtime failed.");
	c->eol[n++] = ' ';
	va_start(vl, fmt);
	s = c->eol + n;
	n += vsprintf(s, fmt, vl);
	va_end(vl);

	if (logfp) {
		fprintf(logfp, "%-12.12s\t%04d-%02d-%02dT%02d:%02d:%02dZ\t%s\n",
			c->name,
			gmtm->tm_year + 1900, gmtm->tm_mon + 1, gmtm->tm_mday,
			gmtm->tm_hour, gmtm->tm_min, gmtm->tm_sec, s);
		fflush(logfp);
	}

	strcat(c->eol, "\n");
	if (n >= LineLen - 1)
		c->eol += LineLen - 1;
	else
		c->eol += n + 1;
	if (cn == ch && c->n == 0) {
		char *p = c->eol - n - 1;

		if (p != c->buf)
			waddch(scr.mw, '\n');
		pushl(p, c->eol - 1);
		wrefresh(scr.mw);
	}
}

static void
scmd(char *usr, char *cmd, char *par, char *data)
{
	int s, c;
	char *pm = strtok(par, " "), *chan;

	if (!usr)
		usr = "?";
	else {
		char *bang = strchr(usr, '!');
		if (bang)
			*bang = 0;
	}
	if (!strcmp(cmd, "PRIVMSG")) {
		if (!pm || !data)
			return;
		if (strchr("&#!+.~", pm[0]))
			chan = pm;
		else
			chan = usr;
		if (!(c = chfind(chan))) {
			if (chadd(chan, 0) < 0)
				return;
			tredraw();
		}
		c = chfind(chan);
		if (strstr(data, nick)) {
			pushf(c, PFMTHIGH, usr, data);
			chl[c].high |= ch != c;
		} else
			pushf(c, PFMT, usr, data);
		if (ch != c) {
			chl[c].new = 1;
			tdrawbar();
		}
	} else if (!strcmp(cmd, "PING")) {
		sndf("PONG :%s", data ? data : "(null)");
	} else if (!strcmp(cmd, "PART")) {
		if (!pm)
			return;
		pushf(chfind(pm), "-!- %s has left %s", usr, pm);
	} else if (!strcmp(cmd, "JOIN")) {
		if (!pm)
			return;
		pushf(chfind(pm), "-!- %s has joined %s", usr, pm);
	} else if (!strcmp(cmd, "470")) { /* Channel forwarding. */
		char *ch = strtok(0, " "), *fch = strtok(0, " ");

		if (!ch || !fch || !(s = chfind(ch)))
			return;
		chl[s].name[0] = 0;
		strncat(chl[s].name, fch, ChanLen - 1);
		tdrawbar();
	} else if (!strcmp(cmd, "471") || !strcmp(cmd, "473")
		   || !strcmp(cmd, "474") || !strcmp(cmd, "475")) { /* Join error. */
		if ((pm = strtok(0, " "))) {
			chdel(pm);
			pushf(0, "-!- Cannot join channel %s (%s)", pm, cmd);
			tredraw();
		}
	} else if (!strcmp(cmd, "QUIT")) { /* Commands we don't care about. */
		return;
	} else if (!strcmp(cmd, "NOTICE") || !strcmp(cmd, "375")
	       || !strcmp(cmd, "372") || !strcmp(cmd, "376")) {
		pushf(0, "%s", data ? data : "");
	} else
		pushf(0, "%s - %s %s", cmd, par, data ? data : "(null)");
}

static void
uparse(char *m)
{
	char *p = m;

	if (!p[0] || (p[1] != ' ' && p[1] != 0)) {
	pmsg:
		if (ch == 0)
			return;
		m += strspn(m, " ");
		if (!*m)
			return;
		pushf(ch, PFMT, nick, m);
		sndf("PRIVMSG %s :%s", chl[ch].name, m);
		return;
	}
	switch (*p) {
	case 'j': /* Join channels. */
		p += 1 + (p[1] == ' ');
		p = strtok(p, " ");
		while (p) {
			if (chadd(p, 1) < 0)
				break;
			sndf("JOIN %s", p);
			p = strtok(0, " ");
		}
		tredraw();
		return;
	case 'l': /* Leave channels. */
		p += 1 + (p[1] == ' ');
		if (!*p) {
			if (ch == 0)
				return; /* Cannot leave server window. */
			strcat(p, chl[ch].name);
		}
		p = strtok(p, " ");
		while (p) {
			if (chdel(p))
				sndf("PART %s", p);
			p = strtok(0, " ");
		}
		tredraw();
		return;
	case 'm': /* Private message. */
		m = p + 1 + (p[1] == ' ');
		if (!(p = strchr(m, ' ')))
			return;
		*p++ = 0;
		sndf("PRIVMSG %s :%s", m, p);
		return;
	case 'r': /* Send raw. */
		if (p[1])
			sndf("%s", &p[2]);
		return;
	case 'q': /* Quit. */
		quit = 1;
		return;
	default: /* Send on current channel. */
		goto pmsg;
	}
}

static void
tinit(void)
{
	setlocale(LC_ALL, "");
	initscr();
	raw();
	noecho();
    timeout(0);
	getmaxyx(stdscr, scr.y, scr.x);
	if (scr.y < 4)
		panic("Screen too small.");
	if ((scr.sw = newwin(1, scr.x, 0, 0)) == 0
	|| (scr.mw = newwin(scr.y - 2, scr.x, 1, 0)) == 0
	|| (scr.iw = newwin(1, scr.x, scr.y - 1, 0)) == 0)
		panic("Cannot create windows.");
	keypad(scr.iw, 1);
	scrollok(scr.mw, 1);
	if (has_colors() == TRUE) {
		start_color();
		init_pair(1, COLOR_WHITE, COLOR_BLUE);
		wbkgd(scr.sw, COLOR_PAIR(1));
	}
}

static void
tredraw(void)
{
	struct Chan *const c = &chl[ch];
	char *q, *p;
	int nl = -1;

	return;

	if (c->eol == c->buf) {
		wclear(scr.mw);
		wrefresh(scr.mw);
		return;
	}
	p = c->eol - 1;
	if (c->n) {
		int i = c->n;
		for (; p > c->buf; p--)
			if (*p == '\n' && !i--)
				break;
		if (p == c->buf)
			c->n -= i;
	}
	q = p;
	while (nl < scr.y - 2) {
		while (*q != '\n' && q > c->buf)
			q--;
		nl++;
		if (q == c->buf)
			break;
		q--;
	}
	if (q != c->buf)
		q += 2;
	wclear(scr.mw);
	wmove(scr.mw, 0, 0);
	while (q < p)
		q = pushl(q, p);
	wrefresh(scr.mw);
}

static void
tdrawbar(void)
{
	size_t l;
	int fst = ch;

	for (l = 0; fst > 0 && l < scr.x / 2; fst--)
		l += strlen(chl[fst].name) + 3;

	werase(scr.sw);
	for (l = 0; fst < nch && l < scr.x; fst++) {
		char *p = chl[fst].name;
		if (fst == ch)
			wattron(scr.sw, A_BOLD);
		waddch(scr.sw, '['), l++;
		if (chl[fst].high)
			waddch(scr.sw, '>'), l++;
		else if (chl[fst].new)
			waddch(scr.sw, '+'), l++;
		for (; *p && l < scr.x; p++, l++)
			waddch(scr.sw, *p);
		if (l < scr.x - 1)
			waddstr(scr.sw, "] "), l += 2;
		if (fst == ch)
			wattroff(scr.sw, A_BOLD);
	}
	wrefresh(scr.sw);
}

static void
tgetch(void)
{
	static char l[BufSz];
	static size_t shft, cu, len;
	size_t dirty = len + 1, i;
	int c;

	c = wgetch(scr.iw);
	switch (c) {
    case ERR:
        return;
	case CTRL('n'):
		ch = (ch + 1) % nch;
		chl[ch].high = chl[ch].new = 0;
		tdrawbar();
		tredraw();
		return;
	case CTRL('p'):
		ch = (ch + nch - 1) % nch;
		chl[ch].high = chl[ch].new = 0;
		tdrawbar();
		tredraw();
		return;
	case KEY_PPAGE:
		chl[ch].n += SCROLL;
		tredraw();
		return;
	case KEY_NPAGE:
		chl[ch].n -= SCROLL;
		if (chl[ch].n < 0)
			chl[ch].n = 0;
		tredraw();
		return;
	case CTRL('a'):
		cu = 0;
		break;
	case CTRL('e'):
		cu = len;
		break;
	case CTRL('b'):
	case KEY_LEFT:
		if (cu)
			cu--;
		break;
	case CTRL('f'):
	case KEY_RIGHT:
		if (cu < len)
			cu++;
		break;
	case CTRL('k'):
		dirty = len = cu;
		break;
	case CTRL('u'):
		if (cu == 0)
			return;
		len -= cu;
		memmove(l, &l[cu], len);
		dirty = cu = 0;
		break;
	case CTRL('d'):
		if (cu >= len)
			return;
		memmove(&l[cu], &l[cu + 1], len - cu - 1);
		dirty = cu;
		len--;
		break;
	case CTRL('h'):
	case KEY_BACKSPACE:
		if (cu == 0)
			return;
		memmove(&l[cu - 1], &l[cu], len - cu);
		dirty = --cu;
		len--;
		break;
	case CTRL('w'):
		if (cu == 0)
			break;
		i = 1;
		while (l[cu - i] == ' ' && cu - i != 0) i++;
		while (l[cu - i] != ' ' && cu - i != 0) i++;
		if (cu - i != 0) i--;
		memmove(&l[cu - i], &l[cu], len - cu);
		cu -= i;
		dirty = cu;
		len -= i;
		break;
	case '\n':
		l[len] = 0;
		uparse(l);
		dirty = cu = len = 0;
		break;
	default:
		if (c > CHAR_MAX || len >= BufSz - 1)
			return; /* Skip other curses codes. */
		memmove(&l[cu + 1], &l[cu], len - cu);
		dirty = cu;
		len++;
		l[cu++] = c;
		break;
	}
	while (cu < shft)
		dirty = 0, shft -= shft >= scr.x / 2 ? scr.x / 2 : shft;
	while (cu >= scr.x + shft)
		dirty = 0, shft += scr.x / 2;
	if (dirty <= shft)
		i = shft;
	else if (dirty > scr.x + shft || dirty > len)
		goto mvcur;
	else
		i = dirty;
	wmove(scr.iw, 0, i - shft);
	wclrtoeol(scr.iw);
	for (; i - shft < scr.x && i < len; i++)
		waddch(scr.iw, l[i]);
mvcur:	wmove(scr.iw, 0, cu - shft);
}

static void
treset(void)
{
	if (scr.mw)
		delwin(scr.mw);
	if (scr.sw)
		delwin(scr.sw);
	if (scr.iw)
		delwin(scr.iw);
	endwin();
}

int
main(int argc, char *argv[])
{
	const char *user = getenv("USER");
	const char *ircnick = getenv("IRCNICK");
	const char *key = getenv("IRCPASS");
	const char *server = SRV;
	const char *port = PORT;
	char *err;
	int o, reconn;

	signal(SIGPIPE, SIG_IGN);
	while ((o = getopt(argc, argv, "thk:n:u:s:p:l:")) >= 0)
		switch (o) {
		case 'h':
		case '?':
		usage:
			fputs("usage: irc [-n NICK] [-u USER] [-s SERVER] [-p PORT] [-l LOGFILE ] [-t] [-h]\n", stderr);
			exit(0);
		case 'l':
			if (!(logfp = fopen(optarg, "a")))
				panic("fopen: logfile");
			break;
		case 'n':
			if (strlen(optarg) >= sizeof nick)
				goto usage;
			strcpy(nick, optarg);
			break;
		case 't':
			ssl = 1;
			break;
		case 'u':
			user = optarg;
			break;
		case 's':
			server = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		}
	if (!user)
		user = "anonymous";
	if (!nick[0] && ircnick && strlen(ircnick) < sizeof nick)
		strcpy(nick, ircnick);
	if (!nick[0] && strlen(user) < sizeof nick)
		strcpy(nick, user);
	if (!nick[0])
		goto usage;
	tinit();
	err = dial(server, port);
	if (err)
		panic(err);
	chadd(server, 0);
	sinit(key, nick, user);
	reconn = 0;
	while (!quit) {
		struct timeval tm;
		struct Chan *c;
		fd_set rfs, wfs;
		int ret;

        memset(&tm, 0, sizeof(tm));
        tm.tv_sec = 5;

		FD_ZERO(&wfs);
		FD_ZERO(&rfs);
		FD_SET(0, &rfs);
		if (!reconn) {
			FD_SET(srv.fd, &rfs);
			if (outp != outb)
				FD_SET(srv.fd, &wfs);
		}
		ret = select(srv.fd + 1, &rfs, &wfs, NULL, (struct timeval *)&tm);
		if (ret < 0) {
			if (!((errno == EINTR) || (errno == EAGAIN)))
			panic("Select failed.");
		}
		if (reconn) {
			hangup();
			if (reconn++ == MaxRecons + 1)
				panic("Link lost.");
			pushf(0, "-!- Link lost, attempting reconnection...");
			if (dial(server, port) != 0)
				continue;
			sinit(key, nick, user);
			for (c = chl; c < &chl[nch]; ++c)
				if (c->join)
					sndf("JOIN %s", c->name);
			reconn = 0;
		}
		if (FD_ISSET(srv.fd, &rfs)) {
			if (!srd()) {
				reconn = 1;
				//continue;
			}
		}
		if (FD_ISSET(srv.fd, &wfs)) {
			int wr;

			wr = write(srv.fd, outb, outp - outb);
			if (wr <= 0) {
				reconn = wr < 0;
				continue;
			}
			outp -= wr;
			memmove(outb, outb + wr, outp - outb);
		}
		if (FD_ISSET(0, &rfs)) {
			tgetch();
			wrefresh(scr.iw);
		}
	}
	hangup();
	while (nch--)
		free(chl[nch].buf);
	treset();
	exit(0);
}
