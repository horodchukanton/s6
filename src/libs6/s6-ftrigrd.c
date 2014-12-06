/* ISC license. */

#include <errno.h>
#include <signal.h>
#include <regex.h>
#include <skalibs/uint16.h>
#include <skalibs/uint32.h>
#include <skalibs/bytestr.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/error.h>
#include <skalibs/strerr2.h>
#include <skalibs/buffer.h>
#include <skalibs/stralloc.h>
#include <skalibs/genalloc.h>
#include <skalibs/bufalloc.h>
#include <skalibs/sig.h>
#include <skalibs/tai.h>
#include <skalibs/djbunix.h>
#include <skalibs/iopause.h>
#include <skalibs/unixmessage.h>
#include <skalibs/skaclient.h>
#include "ftrig1.h"
#include <s6/ftrigr.h>

#define FTRIGRD_MAXREADS 32
#define FTRIGRD_BUFSIZE 17

#define dienomem() strerr_diefu1sys(111, "stralloc_catb")

typedef struct ftrigio_s ftrigio_t, *ftrigio_t_ref ;
struct ftrigio_s
{
  unsigned int xindex ;
  ftrig1_t trig ;
  buffer b ;
  char buf[FTRIGRD_BUFSIZE] ;
  regex_t re ;
  stralloc sa ;
  uint32 options ;
  uint16 id ; /* given by client */
} ;
#define FTRIGIO_ZERO { .xindex = 0, .trig = FTRIG1_ZERO, .b = BUFFER_INIT(0, -1, 0, 0), .buf = "", .sa = STRALLOC_ZERO, .options = 0, .id = 0 }
static ftrigio_t const fzero = FTRIGIO_ZERO ;

static genalloc a = GENALLOC_ZERO ; /* array of ftrigio_t */

static void ftrigio_deepfree (ftrigio_t_ref p)
{
  ftrig1_free(&p->trig) ;
  stralloc_free(&p->sa) ;
  regfree(&p->re) ;
  *p = fzero ;
}

static void cleanup (void)
{
  register unsigned int i = genalloc_len(ftrigio_t, &a) ;
  for (; i ; i--) ftrigio_deepfree(genalloc_s(ftrigio_t, &a) + i - 1) ;
  genalloc_setlen(ftrigio_t, &a, 0) ;
}
 
static void trig (uint16 id, char what, char info)
{
  char pack[4] ;
  unixmessage_t m = { .s = pack, .len = 4, .fds = 0, .nfds = 0 } ;
  uint16_pack_big(pack, id) ;
  pack[2] = what ; pack[3] = info ;
  if (!unixmessage_put(unixmessage_sender_x, &m))
  {
    cleanup() ;
    strerr_diefu1sys(111, "build answer") ;
  }
}

static void answer (char c)
{
  unixmessage_t m = { &c, 1, 0, 0 } ;
  if (!unixmessage_put(unixmessage_sender_1, &m))
  {
    cleanup() ;
    strerr_diefu1sys(111, "unixmessage_put") ;
  }
}

static void remove (unsigned int i)
{
  register unsigned int n = genalloc_len(ftrigio_t, &a) - 1 ;
  ftrigio_deepfree(genalloc_s(ftrigio_t, &a) + i) ;
  genalloc_s(ftrigio_t, &a)[i] = genalloc_s(ftrigio_t, &a)[n] ;
  genalloc_setlen(ftrigio_t, &a, n) ;
}

static inline int ftrigio_read (ftrigio_t *p)
{
  unsigned int n = FTRIGRD_MAXREADS ;
  while (n--)
  {
    regmatch_t pmatch ;
    unsigned int blen ;
    register int r = sanitize_read(buffer_fill(&p->b)) ;
    if (!r) break ;
    if (r < 0) return (trig(p->id, 'd', errno), 0) ;
    blen = buffer_len(&p->b) ;
    if (!stralloc_readyplus(&p->sa, blen+1)) dienomem() ;
    buffer_getnofill(&p->b, p->sa.s + p->sa.len, blen) ;
    p->sa.len += blen ;
    p->sa.s[p->sa.len] = 0 ;
    while (!regexec(&p->re, p->sa.s, 1, &pmatch, REG_NOTBOL | REG_NOTEOL))
    {
      trig(p->id, '!', p->sa.s[pmatch.rm_eo - 1]) ;
      if (!(p->options & FTRIGR_REPEAT)) return 0 ;
      byte_copy(p->sa.s, p->sa.len + 1 - pmatch.rm_eo, p->sa.s + pmatch.rm_eo) ;
      p->sa.len -= pmatch.rm_eo ;
    }
  }
  return 1 ;
}

static int parse_protocol (unixmessage_t const *m, void *context)
{
  uint16 id ;
  if (m->len < 3 || m->nfds)
  {
    cleanup() ;
    strerr_dief1x(100, "invalid client request") ;
  }
  uint16_unpack_big(m->s, &id) ;
  switch (m->s[2])
  {
    case 'U' : /* unsubscribe */
    {
      register unsigned int i = genalloc_len(ftrigio_t, &a) ;
      for (; i ; i--) if (genalloc_s(ftrigio_t, &a)[i-1].id == id) break ;
      if (i) remove(i-1) ;
      answer(0) ;
      break ;
    }
    case 'L' : /* subscribe to path and match re */
    {
      ftrigio_t f = FTRIGIO_ZERO ;
      uint32 pathlen, relen ;
      int r ;
      if (m->len < 18)
      {
        answer(EPROTO) ;
        break ;
      }
      uint32_unpack_big(m->s + 3, &f.options) ;
      uint32_unpack_big(m->s + 7, &pathlen) ;
      uint32_unpack_big(m->s + 11, &relen) ;
      if (((pathlen + relen + 17) != m->len) || m->s[15 + pathlen] || m->s[m->len - 1])
      {
        answer(EPROTO) ;
        break ;
      }
      f.id = id ;
      r = regcomp(&f.re, m->s + 16 + pathlen, REG_EXTENDED) ;
      if (r)
      {
        answer(r == REG_ESPACE ? ENOMEM : EINVAL) ;
        break ;
      }
      if (!ftrig1_make(&f.trig, m->s + 15))
      {
        regfree(&f.re) ;
        answer(errno) ;
        break ;
      }
      if (!buffer_init(&buffer_read, f.fd, f.buf, FTRIGRD_BUFSIZE))
      {
        ftrigio_deepfree(&f) ;
        answer(errno) ;
        break ;
      }
      if (!genalloc_append(ftrigio_t, &a, &f))
      {
        ftrigio_deepfree(&f) ;
        answer(errno) ;
        break ;
      }
      answer(0) ;
      break ;
    }
    default :
    {
      cleanup() ;
      strerr_dief1x(100, "invalid client request") ;
    }
  }
  (void)context ;
  return 1 ;
}

int main (void)
{
  PROG = "s6-ftrigrd" ;

  if (ndelay_on(0) < 0) strerr_diefu2sys(111, "ndelay_on ", "0") ;
  if (ndelay_on(1) < 0) strerr_diefu2sys(111, "ndelay_on ", "1") ;
  if (sig_ignore(SIGPIPE) < 0) strerr_diefu1sys(111, "ignore SIGPIPE") ;

  {
    tain_t deadline ;
    tain_now_g() ;
    tain_addsec_g(&deadline, 2) ;
    if (!skaclient_server_01x_init_g(FTRIGR_BANNER1, FTRIGR_BANNER1_LEN, FTRIGR_BANNER2, FTRIGR_BANNER2_LEN, &deadline))
      strerr_diefu1sys(111, "sync with client") ;
  }

  for (;;)
  {
    register unsigned int n = genalloc_len(ftrigio_t, &a) ;
    iopause_fd x[3 + n] ;
    unsigned int i = 0 ;

    x[0].fd = 0 ; x[0].events = IOPAUSE_EXCEPT | IOPAUSE_READ ;
    x[1].fd = 1 ; x[1].events = IOPAUSE_EXCEPT | (unixmessage_sender_isempty(unixmessage_sender_1) ? 0 : IOPAUSE_WRITE) ;
    x[2].fd = unixmessage_sender_fd(unixmessage_sender_x) ;
    x[2].events = IOPAUSE_EXCEPT | (unixmessage_sender_isempty(unixmessage_sender_x) ? 0 : IOPAUSE_WRITE) ;
    for (; i < n ; i++)
    {
      register ftrigio_t_ref p = genalloc_s(ftrigio_t, &a) + i ;
      p->xindex = 3 + i ;
      x[3+i].fd = p->trig.fd ;
      x[3+i].events = IOPAUSE_READ ;
    }

    if (iopause(x, 3 + n, 0, 0) < 0)
    {
      cleanup() ;
      strerr_diefu1sys(111, "iopause") ;
    }

   /* client closed */
    if ((x[0].revents | x[1].revents) & IOPAUSE_EXCEPT) break ;

   /* client is reading */
    if (x[1].revents & IOPAUSE_WRITE)
      if ((unixmessage_sender_flush(unixmessage_sender_1) < 0) && !error_isagain(errno))
      {
        cleanup() ;
        strerr_diefu1sys(111, "flush stdout") ;
      }
    if (x[2].revents & IOPAUSE_WRITE)
      if ((!unixmessage_sender_flush(unixmessage_sender_x) < 0) && !error_isagain(errno))
      {
        cleanup() ;
        strerr_diefu1sys(111, "flush asyncout") ;
      }

   /* scan listening ftrigs */
    for (i = 0 ; i < genalloc_len(ftrigio_t, &a) ; i++)
    {
      register ftrigio_t_ref p = genalloc_s(ftrigio_t, &a) + i ;
      if (x[p->xindex].revents & IOPAUSE_READ)
        if (!ftrigio_read(p)) remove(i--) ;
    }

   /* client is writing */
    if (!unixmessage_receiver_isempty(unixmessage_receiver_0) || x[0].revents & IOPAUSE_READ)
    {
      if (unixmessage_handle(unixmessage_receiver_0, &parse_protocol, 0) < 0)
      {
        if (errno == EPIPE) break ; /* normal exit */
        cleanup() ;
        strerr_diefu1sys(111, "handle messages from client") ;
      }
    }
  }
  cleanup() ;
  return 0 ;
}
