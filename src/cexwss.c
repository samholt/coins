#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#undef EV_COMPAT3
#include <ev.h>
#include <openssl/hmac.h>
#include "ws.h"
#include "nifty.h"
#include "../cex_cred.h"

static const char logfile[] = "prices";
static char hostname[256];
static size_t hostnsz;

#define API_URL		"wss://ws.cex.io/ws"

#define TIMEOUT		6.0
#define NTIMEOUTS	10
#define ONE_DAY		86400.0
#define MIDNIGHT	0.0
#define ONE_WEEK	604800.0
#define SATURDAY	172800.0
#define SUNDAY		302400.0

/* number of seconds we tolerate inactivity in the beef channels */
#define MAX_INACT	(30)

#define strlenof(x)	(sizeof(x) - 1U)

#if defined __INTEL_COMPILER
# pragma warning (disable:2259)
#endif	/* __INTEL_COMPILER */
#if !defined UNUSED
# define UNUSED(_x)	__attribute__((unused)) __##_x
#endif	/* !UNUSED */
#define EV_PU	EV_P __attribute__((unused))
#define EV_PU_	EV_PU,

typedef struct coin_ctx_s *coin_ctx_t;
typedef struct timespec *timespec_t;

typedef enum {
	COIN_ST_UNK,
	COIN_ST_CONN,
	COIN_ST_CONND,
	COIN_ST_JOIN,
	COIN_ST_JOIND,
	COIN_ST_SLEEP,
	COIN_ST_NODATA,
	COIN_ST_RECONN,
	COIN_ST_INTR,
} coin_st_t;

struct coin_ctx_s {
	/* libev's idea of the socket below */
	ev_io watcher[1];
	ev_timer timer[1];
	ev_timer pongt[1];
	ev_signal sigi[1];
	ev_signal sigp[1];
	ev_signal sigh[1];
	ev_periodic midnight[1];
	ev_prepare prep[1];

	/* keep track of heart beats */
	int nothing;
	/* ssl context */
	ws_t ws;
	/* internal state */
	coin_st_t st;

	/* subs */
	const char *const *subs;
	size_t nsubs;

	struct timespec last_act[1];
};

/* always have room for the timestamp */
#define INI_GBOF	21U
static char gbuf[1048576U];
static size_t gbof = INI_GBOF;
static int logfd;


#define countof(x)	(sizeof(x) / sizeof(*x))

static __attribute__((format(printf, 1, 2))) void
serror(const char *fmt, ...)
{
	va_list vap;
	va_start(vap, fmt);
	vfprintf(stderr, fmt, vap);
	va_end(vap);
	if (errno) {
		fputs(": ", stderr);
		fputs(strerror(errno), stderr);
	}
	fputc('\n', stderr);
	return;
}

static __attribute__((unused)) inline void
fputsl(FILE *fp, const char *s, size_t l)
{
	for (size_t i = 0; i < l; i++) {
		fputc_unlocked(s[i], fp);
	}
	fputc_unlocked('\n', fp);
	return;
}

static struct timespec tsp[1];

static inline size_t
hrclock_print(char *buf, size_t __attribute__((unused)) len)
{
	clock_gettime(CLOCK_REALTIME_COARSE, tsp);
	return sprintf(buf, "%ld.%09li", tsp->tv_sec, tsp->tv_nsec);
}

static void
linger_sock(int sock)
{
#if defined SO_LINGER
	struct linger lng[1] = {{.l_onoff = 1, .l_linger = 1}};
	(void)setsockopt(sock, SOL_SOCKET, SO_LINGER, lng, sizeof(*lng));
#endif	/* SO_LINGER */
	return;
}

static int
close_sock(int fd)
{
	linger_sock(fd);
	fdatasync(fd);
	shutdown(fd, SHUT_RDWR);
	return close(fd);
}

static char*
xmemmem(const char *hay, const size_t hayz, const char *ndl, const size_t ndlz)
{
	const char *const eoh = hay + hayz;
	const char *const eon = ndl + ndlz;
	const char *hp;
	const char *np;
	const char *cand;
	unsigned int hsum;
	unsigned int nsum;
	unsigned int eqp;

	/* trivial checks first
         * a 0-sized needle is defined to be found anywhere in haystack
         * then run strchr() to find a candidate in HAYSTACK (i.e. a portion
         * that happens to begin with *NEEDLE) */
	if (ndlz == 0UL) {
		return deconst(hay);
	} else if ((hay = memchr(hay, *ndl, hayz)) == NULL) {
		/* trivial */
		return NULL;
	}

	/* First characters of haystack and needle are the same now. Both are
	 * guaranteed to be at least one character long.  Now computes the sum
	 * of characters values of needle together with the sum of the first
	 * needle_len characters of haystack. */
	for (hp = hay + 1U, np = ndl + 1U, hsum = *hay, nsum = *hay, eqp = 1U;
	     hp < eoh && np < eon;
	     hsum ^= *hp, nsum ^= *np, eqp &= *hp == *np, hp++, np++);

	/* HP now references the (NZ + 1)-th character. */
	if (np < eon) {
		/* haystack is smaller than needle, :O */
		return NULL;
	} else if (eqp) {
		/* found a match */
		return deconst(hay);
	}

	/* now loop through the rest of haystack,
	 * updating the sum iteratively */
	for (cand = hay; hp < eoh; hp++) {
		hsum ^= *cand++;
		hsum ^= *hp;

		/* Since the sum of the characters is already known to be
		 * equal at that point, it is enough to check just NZ - 1
		 * characters for equality,
		 * also CAND is by design < HP, so no need for range checks */
		if (hsum == nsum && memcmp(cand, ndl, ndlz - 1U) == 0) {
			return deconst(cand);
		}
	}
	return NULL;
}

static size_t
hmac(
	char *restrict buf, size_t bsz,
	const char *msg, size_t len,
	const char *key, size_t ksz)
{
	unsigned int hlen = bsz;
	unsigned char *sig = HMAC(
		EVP_sha256(), key, ksz,
		(const unsigned char*)msg, len,
		NULL, &hlen);

	/* hexl him */
	for (size_t i = 0U; i < hlen * 2U; i += 2U) {
		buf[i + 0U] = (char)((unsigned char)(sig[i / 2U] >> 4U) & 0xfU);
		buf[i + 1U] = (char)((unsigned char)(sig[i / 2U] >> 0U) & 0xfU);
	}
	for (size_t i = 0U; i < hlen * 2U; i++) {
		if ((unsigned char)buf[i] < 10U) {
			buf[i] ^= '0';
		} else {
			buf[i] += 'W';
		}
	}
	return hlen;
}


static inline size_t
memncpy(char *restrict tgt, const char *src, size_t zrc)
{
	memcpy(tgt, src, zrc);
	return zrc;
}

static inline size_t
memnmove(char *tgt, const char *src, size_t zrc)
{
	memmove(tgt, src, zrc);
	return zrc;
}

static int
loghim(const char *buf, size_t len)
{
	size_t prfz;

	/* this is a prefix that we prepend to each line */
	prfz = hrclock_print(gbuf, INI_GBOF);
	gbuf[prfz++] = '\t';

	memnmove(gbuf + prfz, buf, len);
	gbuf[prfz + len++] = '\n';
	write(logfd, gbuf, prfz + len);
	fwrite(gbuf, 1, prfz + len, stderr);
	return 0;
}

static ssize_t
logwss(const char *buf, size_t len)
{
	const char *lp = buf;
	size_t prfz;

	/* this is a prefix that we prepend to each line */
	prfz = hrclock_print(gbuf, INI_GBOF);
	gbuf[prfz++] = '\t';

	for (const char *eol, *const ep = buf + len;
	     lp < ep && (eol = memchr(lp, '\n', ep - lp));
	     lp = eol + 1U) {
		memmove(gbuf + prfz, lp, eol - lp);
		gbuf[prfz + (eol - lp)] = '\n';
		write(logfd, gbuf, prfz + (eol - lp) + 1U);
		fwrite(gbuf, 1, prfz + (eol - lp) + 1U, stderr);
	}
	return lp - buf;
}


typedef struct coin_data_s {
	struct tm tm[1];
	char *bid;
	size_t blen;
	char *ask;
	size_t alen;
	char *tra;
	size_t tlen;
} *coin_data_t;

static void
open_outfile(void)
{
	if ((logfd = open(logfile, O_WRONLY | O_CREAT, 0644)) < 0) {
		serror("cannot open outfile `%s'", logfile);
		exit(EXIT_FAILURE);
	}
	/* coinw to the end */
	lseek(logfd, 0, SEEK_END);
	return;
}

static void
rotate_outfile(void)
{
	static char msg[] = "rotate...midnight";
	struct tm tm[1];
	char new[256], *n = new;
	time_t now;

	/* get a recent time stamp */
	now = time(NULL);
	gmtime_r(&now, tm);
	n += memncpy(n, logfile, strlenof(logfile));
	*n++ = '-';
	strncpy(n, hostname, hostnsz);
	n += hostnsz;
	*n++ = '-';
	strftime(n, sizeof(new) - strlenof(logfile) - hostnsz,
		 "%Y-%m-%dT%H:%M:%S", tm);

	fprintf(stderr, "new \"%s\"\n", new);

	/* close the old file */
	loghim(msg, sizeof(msg) - 1);
	close_sock(logfd);
	/* rename it and reopen under the old name */
	rename(logfile, new);
	open_outfile();
	return;
}


static void pong(coin_ctx_t ctx);

static void
ws_cb(EV_P_ ev_io *w, int UNUSED(revents))
{
/* we know that w is part of the coin_ctx_s structure */
	coin_ctx_t ctx = w->data;
	size_t maxr = sizeof(gbuf) - gbof;
	ssize_t nrd;

	if ((nrd = ws_recv(ctx->ws, gbuf + gbof, maxr, 0)) <= 0) {
		/* connexion reset or something? */
		serror("recv(%d) failed, read %zi", w->fd, nrd);
		goto unroll;
	}

#if 1
/* debugging */
	fprintf(stderr, "WS (%u) read %zu+%zi/%zu bytes\n", ctx->st, gbof, nrd, maxr);
#endif	/* 1 */

	switch (ctx->st) {
		ssize_t npr;

	case COIN_ST_CONN:
		ctx->st = COIN_ST_CONND;
	case COIN_ST_CONND:
		fputs("CONND\n", stderr);
		gbof = INI_GBOF;
		break;

	case COIN_ST_JOIN:
		/* assume that we've successfully joined */
		ctx->st = COIN_ST_JOIND;
		ev_timer_start(EV_A_ ctx->pongt);
	case COIN_ST_JOIND:;
		gbof += nrd;
		/* log him */
		npr = logwss(gbuf + INI_GBOF, gbof - INI_GBOF);

		/* check for high level pings */
		if (xmemmem(gbuf + INI_GBOF, gbof - INI_GBOF, "ping", 4U)) {
			pong(ctx);
		}

		memnmove(gbuf + INI_GBOF, gbuf + INI_GBOF + npr, gbof - npr);
		gbof -= npr;

		/* keep a reference of our time stamp */
		*ctx->last_act = *tsp;
		break;

	default:
		gbof = INI_GBOF;
		break;
	}

	ev_timer_again(EV_A_ ctx->timer);
	ctx->nothing = 0;
	return;

unroll:
	/* connection reset */
	loghim("restart in 9", 12);
	sleep(1);
	loghim("restart in 8", 12);
	sleep(1);
	loghim("restart in 7", 12);
	sleep(1);
	loghim("restart in 6", 12);
	sleep(1);
	loghim("restart in 5", 12);
	sleep(1);
	loghim("restart in 4", 12);
	sleep(1);
	loghim("restart in 3", 12);
	sleep(1);
	loghim("restart in 2", 12);
	sleep(1);
	loghim("restart in 1", 12);
	sleep(1);
	loghim("restart", 7);
	ctx->nothing = 0;
	ctx->st = COIN_ST_RECONN;
	return;
}

static void
midnight_cb(EV_PU_ ev_periodic *UNUSED(w), int UNUSED(r))
{
	rotate_outfile();
	return;
}

static void
sighup_cb(EV_PU_ ev_signal *w, int UNUSED(r))
{
	coin_ctx_t ctx = w->data;
	rotate_outfile();
	ctx->st = COIN_ST_RECONN;
	return;
}

static void
silence_cb(EV_PU_ ev_timer *w, int UNUSED(revents))
{
	coin_ctx_t ctx = w->data;

	loghim("nothing", 7);
	if (ctx->nothing++ >= NTIMEOUTS) {
		switch (ctx->st) {
		case COIN_ST_SLEEP:
			ctx->nothing = 0;
			loghim("wakey wakey", 11);
			ctx->st = COIN_ST_RECONN;
			break;
		default:
			/* only fall asleep when subscribed */
			ctx->nothing = 0;
			loghim("suspend", 7);
			ctx->st = COIN_ST_NODATA;
			break;
		}
	}
	return;
}

static void
pong_cb(EV_PU_ ev_timer *w, int UNUSED(revents))
{
	static void pong(coin_ctx_t ctx);
	coin_ctx_t ctx = w->data;

	pong(ctx);
	return;
}

static void
sigint_cb(EV_PU_ ev_signal *w, int UNUSED(revents))
{
	coin_ctx_t ctx = w->data;
	/* quit the whole shebang */
	loghim("C-c", 3);
	ctx->nothing = 0;
	ctx->st = COIN_ST_INTR;
	return;
}


static void
subscr_coin(EV_P_ coin_ctx_t ctx)
{
#define AUTH	"{\
\"e\":\"auth\",\"auth\":{\"key\":\"" API_KEY "\",\
\"timestamp\":%ld,\
\"signature\":\"%.*s\"}\
}"
#define MREQ	"{\
\"e\":\"order-book-subscribe\",\
\"data\":{\"pair\":[\"%s\",\"%s\"],\"subscribe\":true,\"depth\":0}\
}"
#define TREQ	"{\
\"e\":\"subscribe\",\
\"rooms\":[\"tickers\"]\
}"

	static char buf[256U];
	static char tok[] = "\0\0\0\0\0\0\0\0\0\0" API_KEY;
	/* signature is HMAC-rsa256(tst+key) */
	static char sig[64U];

	/* prepare the login signature */
	with (time_t t = time(NULL)) {
		int len = snprintf(tok, sizeof(tok), "%ld", t);
		if (len < 0) {
			return;
		}
		len += memncpy(tok + len, API_KEY, strlenof(API_KEY));
		hmac(sig, sizeof(sig), tok, len, API_SEC, strlenof(API_SEC));

		len = snprintf(buf, sizeof(buf), AUTH "\r\n", t, 64, sig);
		ws_send(ctx->ws, buf, len, 0);
	}

	with (size_t len = memncpy(buf, TREQ "\r\n", strlenof(TREQ "\r\n"))) {
		ws_send(ctx->ws, buf, len, 0);
	}

	for (size_t i = 0U; i < ctx->nsubs; i++) {
		int len = snprintf(buf, sizeof(buf), MREQ "\r\n",
				   ctx->subs[i] + 0U, ctx->subs[i] + 4U);
		ws_send(ctx->ws, buf, len, 0);
	}

	/* reset nothing counter and start the nothing timer */
	ctx->nothing = 0;
	ev_timer_again(EV_A_ ctx->timer);
	gbof = INI_GBOF;

	ctx->st = COIN_ST_JOIN;
	/* initialise our last activity stamp */
	*ctx->last_act = *tsp;
	return;
}

static void
init_coin(EV_P_ coin_ctx_t ctx)
{
/* this init process is two part: request a token, then do the subscriptions */
	ctx->st = COIN_ST_UNK;
	gbof = INI_GBOF;

	fprintf(stderr, "INIT\n");
	ev_timer_again(EV_A_ ctx->timer);
	if ((ctx->ws = ws_open(API_URL)) == NULL) {
			serror("\
Error: cannot connect");
		/* retry soon, we just use the watcher for this */
		ctx->st = COIN_ST_SLEEP;
		return;
	}

	ev_io_init(ctx->watcher, ws_cb, ws_fd(ctx->ws), EV_READ);
	ev_io_start(EV_A_ ctx->watcher);
	ctx->watcher->data = ctx;
	ctx->st = COIN_ST_CONN;
	return;
}

static void
deinit_coin(EV_P_ coin_ctx_t ctx)
{
	fprintf(stderr, "DEINIT\n");
	/* stop the watcher */
	ev_io_stop(EV_A_ ctx->watcher);

	/* shutdown the network socket */
	if (ctx->ws != NULL) {
		ws_close(ctx->ws);
	}
	ctx->ws = NULL;

	/* set the state to unknown */
	ctx->st = COIN_ST_UNK;
	gbof = INI_GBOF;
	return;
}

static void
reinit_coin(EV_P_ coin_ctx_t ctx)
{
	fprintf(stderr, "REINIT\n");
	deinit_coin(EV_A_ ctx);
	init_coin(EV_A_ ctx);
	return;
}

static void
pong(coin_ctx_t ctx)
{
	static const char pong[] = "{\"e\":\"pong\"}\r\n";

	ws_send(ctx->ws, pong, strlenof(pong), 0);
	return;
}


/* only cb's we allow here */
static void
prepare(EV_P_ ev_prepare *w, int UNUSED(revents))
{
	coin_ctx_t ctx = w->data;

	fprintf(stderr, "PREP(%u)\n", ctx->st);
	switch (ctx->st) {
	case COIN_ST_UNK:
		/* initialise everything, sets the state */
		init_coin(EV_A_ ctx);
		if (ctx->st != COIN_ST_CONN) {
			break;
		}
	case COIN_ST_CONND:
		/* waiting for that HTTP 101 */
		subscr_coin(EV_A_ ctx);
		break;

	case COIN_ST_JOIN:
		break;
	case COIN_ST_JOIND:
		/* check if there's messages from the channel */
		if (tsp->tv_sec - ctx->last_act->tv_sec >= MAX_INACT) {
			goto unroll;
		}
		break;

	case COIN_ST_NODATA:
		fprintf(stderr, "NODATA -> RECONN\n");
	case COIN_ST_RECONN:
		fprintf(stderr, "reconnection requested\n");
		reinit_coin(EV_A_ ctx);
		break;
	case COIN_ST_INTR:
		/* disconnect and unroll */
		deinit_coin(EV_A_ ctx);
		ev_break(EV_A_ EVBREAK_ALL);
		break;
	case COIN_ST_SLEEP:
	default:
		break;
	}
	return;

unroll:
	/* connection reset */
	loghim("restart in 3", 12);
	sleep(1);
	loghim("restart in 2", 12);
	sleep(1);
	loghim("restart in 1", 12);
	sleep(1);
	loghim("restart", 7);
	ctx->nothing = 0;
	ctx->st = COIN_ST_RECONN;
	return;
}

static void
init_ev(EV_P_ coin_ctx_t ctx)
{
	ev_signal_init(ctx->sigi, sigint_cb, SIGINT);
	ev_signal_start(EV_A_ ctx->sigi);
	ev_signal_init(ctx->sigp, sigint_cb, SIGPIPE);
	ev_signal_start(EV_A_ ctx->sigp);
	ctx->sigi->data = ctx;
	ctx->sigp->data = ctx;

	/* inc nothing counter every 3 seconds */
	ev_timer_init(ctx->timer, silence_cb, 0.0, TIMEOUT);
	ctx->timer->data = ctx;
	ev_timer_init(ctx->pongt, pong_cb, TIMEOUT, TIMEOUT);
	ctx->pongt->data = ctx;

	/* the midnight tick for file rotation, also upon sighup */
	ev_periodic_init(ctx->midnight, midnight_cb, MIDNIGHT, ONE_DAY, NULL);
	ev_periodic_start(EV_A_ ctx->midnight);
	ev_signal_init(ctx->sigh, sighup_cb, SIGHUP);
	ev_signal_start(EV_A_ ctx->sigh);
	ctx->midnight->data = ctx;
	ctx->sigh->data = ctx;

	/* prepare and check cbs */
	ev_prepare_init(ctx->prep, prepare);
	ev_prepare_start(EV_A_ ctx->prep);
	ctx->prep->data = ctx;
	return;
}

static void
deinit_ev(EV_P_ coin_ctx_t ctx)
{
	ev_timer_stop(EV_A_ ctx->timer);
	ev_timer_stop(EV_A_ ctx->pongt);
	ev_signal_stop(EV_A_ ctx->sigi);
	ev_signal_stop(EV_A_ ctx->sigp);

	ev_signal_stop(EV_A_ ctx->sigh);
	ev_periodic_stop(EV_A_ ctx->midnight);

	ev_prepare_stop(EV_A_ ctx->prep);
	return;
}


#include "cexwss.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	struct coin_ctx_s ctx[1U];
	int rc = EXIT_SUCCESS;

	if (yuck_parse(argi, argc, argv) < 0) {
		return 1;
	}

	EV_P = ev_default_loop(0);

	/* rinse */
	memset(ctx, 0, sizeof(*ctx));
	/* open our outfile */
	open_outfile();
	/* put the hostname behind logfile */
	(void)gethostname(hostname, sizeof(hostname));
	hostnsz = strlen(hostname);

	/* check symbols */
	for (size_t i = 0U; i < argi->nargs; i++) {
		if (argi->args[i][3U] != '/' && argi->args[i][3U] != ':') {
			errno = 0, serror("\
Error: specify pairs like CCY:CCY");
			rc = EXIT_FAILURE;
			goto out;
		}
		/* split string in two halves */
		argi->args[i][3U] = '\0';
	}
	/* make sure we won't forget them subscriptions */
	ctx->subs = argi->args;
	ctx->nsubs = argi->nargs;

	/* and initialise the libev part of this project */
	init_ev(EV_A_ ctx);

	/* obtain a loop */
	{
		/* work */
		ev_run(EV_A_ 0);
	}

	/* hm? */
	deinit_ev(EV_A_ ctx);
	ev_loop_destroy(EV_DEFAULT_UC);

out:
	/* that's it */
	close_sock(logfd);
	yuck_free(argi);
	return rc;
}

/* coin.c ends here */
