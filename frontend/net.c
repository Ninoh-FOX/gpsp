/*
 * Netplay for the core's netpacket interface, between standalone consoles.
 *
 * It does for the core what RetroArch's netplay does for a netpacket core
 * (network/netplay/netplay_frontend.c): carries its packets and nothing
 * else.
 *  - The host calls start(0) when it starts hosting; a client calls
 *    start(its number) once the host has let it in.
 *  - The host asks the core with connected(n) whether client n may join,
 *    and tells it disconnected(n) when it leaves. stop() ends a session.
 *  - Clients only talk to the host. The host gets what is meant for it
 *    (or for everybody) and passes on what is meant for the others; a
 *    client gets each packet with the number of the console that sent it.
 *
 * TCP (in order, reliable, Nagle off), one frame per packet:
 *   u32 length, u16 address, u16 kind, then the payload (little endian).
 * The address is where the packet goes when a client sends it, and where it
 * comes from when the host passes it on. Kinds: data, hello (client ->
 * host: the core's protocol version), welcome (host -> client: its number
 * as the address), refused (payload: why).
 *
 * Consoles looking for a game ask the local networks over UDP broadcast;
 * a host answers with the game it runs.
 */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "frontend/net.h"

enum { PK_DATA = 0, PK_HELLO = 1, PK_WELCOME = 2, PK_REFUSED = 3 };

#define HDR_LEN        8
#define MAX_PACKET     (1u << 20)     /* a dual cable save state fits */
#define CONNECT_MS     5000
#define DISCOVER_ASK   "GPNP?"
#define DISCOVER_ANS   "GPNP!"

typedef struct {
  int fd;
  int id;               /* 1..3 once in the session, 0 before */
  uint8_t *in;  size_t in_len,  in_cap;
  uint8_t *out; size_t out_len, out_cap;
} conn_t;

static const struct retro_netpacket_callback *core;
static net_state_t state = NET_OFF;
static int listen_fd = -1, udp_fd = -1;
static conn_t conns[NET_MAX_CLIENTS];   /* host: one per client; client: [0] = host */
static int my_id;
static unsigned long connect_start;
static char peer_ip[16];
static char game_name[48] = "gpSP";
static int in_poll;

static unsigned long now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000);
}

static void set_nonblock(int fd)
{
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* ---------------------------------------------------------------- connections */

static void conn_close(conn_t *c)
{
  if (c->fd >= 0)
    close(c->fd);
  free(c->in);
  free(c->out);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static int buf_reserve(uint8_t **buf, size_t *cap, size_t need)
{
  uint8_t *n;
  size_t size = *cap ? *cap : 4096;
  if (need <= *cap)
    return 0;
  while (size < need)
    size *= 2;
  n = realloc(*buf, size);
  if (!n)
    return -1;
  *buf = n;
  *cap = size;
  return 0;
}

/* writes what it can now, keeps the rest for later; -1 = connection gone */
static int conn_flush(conn_t *c)
{
  while (c->out_len) {
    ssize_t n = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return 0;
      if (errno == EINTR)
        continue;
      return -1;
    }
    memmove(c->out, c->out + n, c->out_len - n);
    c->out_len -= n;
  }
  return 0;
}

static int conn_send(conn_t *c, uint16_t kind, uint16_t addr, const void *buf, size_t len)
{
  uint8_t *p;
  if (c->fd < 0 || buf_reserve(&c->out, &c->out_cap, c->out_len + HDR_LEN + len))
    return -1;
  p = c->out + c->out_len;
  put32(p, (uint32_t)len);
  put16(p + 4, addr);
  put16(p + 6, kind);
  if (len)
    memcpy(p + HDR_LEN, buf, len);
  c->out_len += HDR_LEN + len;
  /* as RetroArch with RETRO_NETPACKET_FLUSH_HINT: out at once */
  return conn_flush(c);
}

/* reads what has arrived; -1 = connection gone */
static int conn_read(conn_t *c)
{
  for (;;) {
    ssize_t n;
    if (buf_reserve(&c->in, &c->in_cap, c->in_len + 16384))
      return -1;
    n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
    if (n > 0) {
      c->in_len += n;
      continue;
    }
    if (n == 0)
      return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    if (errno != EINTR)
      return -1;
  }
}

/* next whole packet in c->in: 1 = got one (payload in *buf, free with
 * conn_pop), 0 = not yet, -1 = garbage */
static int conn_peek(conn_t *c, uint16_t *kind, uint16_t *addr, uint8_t **buf, uint32_t *len)
{
  if (c->in_len < HDR_LEN)
    return 0;
  *len = get32(c->in);
  if (*len > MAX_PACKET)
    return -1;
  if (c->in_len < HDR_LEN + *len)
    return 0;
  *addr = get16(c->in + 4);
  *kind = get16(c->in + 6);
  *buf = c->in + HDR_LEN;
  return 1;
}

static void conn_pop(conn_t *c, uint32_t len)
{
  memmove(c->in, c->in + HDR_LEN + len, c->in_len - HDR_LEN - len);
  c->in_len -= HDR_LEN + len;
}

/* ---------------------------------------------------------------- the core's side */

static conn_t *host_conn_by_id(int id)
{
  int i;
  for (i = 0; i < NET_MAX_CLIENTS; i++)
    if (conns[i].fd >= 0 && conns[i].id == id)
      return &conns[i];
  return NULL;
}

static void send_cb(int flags, const void *buf, size_t len, uint16_t client_id)
{
  int i;
  (void)flags;   /* TCP: always reliable and in order */

  if (state == NET_HOSTING) {
    if (client_id == RETRO_NETPACKET_BROADCAST) {
      for (i = 0; i < NET_MAX_CLIENTS; i++)
        if (conns[i].fd >= 0 && conns[i].id)
          conn_send(&conns[i], PK_DATA, 0, buf, len);
    } else if (client_id) {
      conn_t *c = host_conn_by_id(client_id);
      if (c)
        conn_send(c, PK_DATA, 0, buf, len);
    }
  } else if (state == NET_CONNECTED) {
    /* to the host, which passes it on */
    conn_send(&conns[0], PK_DATA, client_id, buf, len);
  }
}

static void poll_cb(void)
{
  net_poll();
}

/* ---------------------------------------------------------------- host */

static void host_drop(conn_t *c, const char *why)
{
  int id = c->id;
  conn_close(c);
  if (id && core && core->disconnected)
    core->disconnected((uint16_t)id);
  if (id && why) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Player %d %s", id + 1, why);
    net_message(msg, 3000);
  }
}

static void host_hello(conn_t *c, const uint8_t *buf, uint32_t len)
{
  const char *ver = core && core->protocol_version ? core->protocol_version : "";
  char msg[64];
  int id;

  if (len != strlen(ver) || memcmp(buf, ver, len)) {
    static const char why[] = "different gpSP version";
    conn_send(c, PK_REFUSED, 0, why, sizeof(why) - 1);
    conn_close(c);
    net_message("Refused a console: different gpSP version", 3000);
    return;
  }
  /* RetroArch numbers a client by its connection: the lowest free one */
  for (id = 1; id <= NET_MAX_CLIENTS; id++)
    if (!host_conn_by_id(id))
      break;
  if (id > NET_MAX_CLIENTS || (core->connected && !core->connected((uint16_t)id))) {
    static const char why[] = "the game is full";
    conn_send(c, PK_REFUSED, 0, why, sizeof(why) - 1);
    conn_close(c);
    return;
  }
  c->id = id;
  conn_send(c, PK_WELCOME, (uint16_t)id, NULL, 0);
  snprintf(msg, sizeof(msg), "Player %d joined", id + 1);
  net_message(msg, 3000);
}

static void host_data(conn_t *c, uint16_t to, const uint8_t *buf, uint32_t len)
{
  int i, from = c->id;

  if (!from)      /* not in the session yet: dropped, as RetroArch does */
    return;
  if (to == RETRO_NETPACKET_BROADCAST || to == 0) {
    if (core->receive)
      core->receive(buf, len, (uint16_t)from);
  }
  if (to == RETRO_NETPACKET_BROADCAST) {
    for (i = 0; i < NET_MAX_CLIENTS; i++)
      if (&conns[i] != c && conns[i].fd >= 0 && conns[i].id)
        conn_send(&conns[i], PK_DATA, (uint16_t)from, buf, len);
  } else if (to && to != from) {
    conn_t *d = host_conn_by_id(to);
    if (d)
      conn_send(d, PK_DATA, (uint16_t)from, buf, len);
  }
}

static void host_answer_discovery(void)
{
  uint8_t buf[64];
  struct sockaddr_in from;
  socklen_t fl = sizeof(from);
  ssize_t n;

  while ((n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl)) > 0) {
    char ans[96];
    int len;
    if (n != sizeof(DISCOVER_ASK) - 1 || memcmp(buf, DISCOVER_ASK, n))
      continue;
    len = snprintf(ans, sizeof(ans), DISCOVER_ANS "%d|%s", net_clients() + 1, game_name);
    sendto(udp_fd, ans, len, 0, (struct sockaddr *)&from, fl);
    fl = sizeof(from);
  }
}

static void host_poll(void)
{
  int i, fd;

  while ((fd = accept(listen_fd, NULL, NULL)) >= 0) {
    int one = 1;
    for (i = 0; i < NET_MAX_CLIENTS; i++)
      if (conns[i].fd < 0)
        break;
    if (i == NET_MAX_CLIENTS) {
      close(fd);
      continue;
    }
    set_nonblock(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    conns[i].fd = fd;
    conns[i].id = 0;
  }

  for (i = 0; i < NET_MAX_CLIENTS; i++) {
    conn_t *c = &conns[i];
    uint16_t kind, addr;
    uint8_t *buf;
    uint32_t len;
    int r;

    if (c->fd < 0)
      continue;
    if (conn_read(c) < 0 || conn_flush(c) < 0) {
      host_drop(c, "left");
      continue;
    }
    while (c->fd >= 0 && (r = conn_peek(c, &kind, &addr, &buf, &len)) != 0) {
      if (r < 0) {
        host_drop(c, "sent garbage");
        break;
      }
      if (kind == PK_HELLO && !c->id)
        host_hello(c, buf, len);
      else if (kind == PK_DATA)
        host_data(c, addr, buf, len);
      if (c->fd >= 0)
        conn_pop(c, len);
    }
  }

  if (udp_fd >= 0)
    host_answer_discovery();
}

int net_host(void)
{
  struct sockaddr_in a;
  int one = 1;

  if (!core || state != NET_OFF)
    return -1;

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0)
    return -1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(NET_PORT);
  if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) || listen(listen_fd, 4)) {
    close(listen_fd);
    listen_fd = -1;
    return -1;
  }
  set_nonblock(listen_fd);

  /* answers the consoles looking for a game (not fatal if it fails) */
  udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_fd >= 0) {
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(udp_fd, (struct sockaddr *)&a, sizeof(a))) {
      close(udp_fd);
      udp_fd = -1;
    } else
      set_nonblock(udp_fd);
  }

  state = NET_HOSTING;
  my_id = 0;
  if (core->start)
    core->start(0, send_cb, poll_cb);
  net_message("Hosting: waiting for players", 3000);
  return 0;
}

/* ---------------------------------------------------------------- client */

static void client_lost(const char *why)
{
  int was_in = (state == NET_CONNECTED);
  conn_close(&conns[0]);
  state = NET_OFF;
  if (was_in && core && core->stop)
    core->stop();
  if (why)
    net_message(why, 3000);
}

static void client_poll(void)
{
  conn_t *c = &conns[0];
  uint16_t kind, addr;
  uint8_t *buf;
  uint32_t len;
  int r;

  if (state == NET_CONNECTING && c->out_len == 0 && !c->in_len) {
    /* still connecting? */
    int err = 0;
    socklen_t el = sizeof(err);
    fd_set w;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&w);
    FD_SET(c->fd, &w);
    if (select(c->fd + 1, NULL, &w, NULL, &tv) <= 0) {
      if (now_ms() - connect_start > CONNECT_MS)
        client_lost("Could not reach the host");
      return;
    }
    getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) {
      client_lost("Could not reach the host");
      return;
    }
    if (!c->id) {
      const char *ver = core->protocol_version ? core->protocol_version : "";
      c->id = -1;   /* hello sent */
      conn_send(c, PK_HELLO, 0, ver, strlen(ver));
    }
  }

  if (conn_read(c) < 0 || conn_flush(c) < 0) {
    client_lost(state == NET_CONNECTED ? "The host left" : "Could not join the game");
    return;
  }
  if (state == NET_CONNECTING && now_ms() - connect_start > CONNECT_MS) {
    client_lost("The host did not answer");
    return;
  }

  while (c->fd >= 0 && (r = conn_peek(c, &kind, &addr, &buf, &len)) != 0) {
    if (r < 0) {
      client_lost("Bad data from the host");
      return;
    }
    if (kind == PK_WELCOME && state == NET_CONNECTING) {
      char msg[64];
      my_id = addr;
      state = NET_CONNECTED;
      snprintf(msg, sizeof(msg), "Joined as player %d", my_id + 1);
      net_message(msg, 3000);
      conn_pop(c, len);
      if (core->start)
        core->start((uint16_t)my_id, send_cb, poll_cb);
      continue;
    }
    if (kind == PK_REFUSED) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Refused: %.*s", (int)(len < 40 ? len : 40), (const char *)buf);
      client_lost(msg);
      return;
    }
    if (kind == PK_DATA && state == NET_CONNECTED && core->receive) {
      /* (kept in the buffer until the core is done with it) */
      core->receive(buf, len, addr);
      if (state != NET_CONNECTED || c->fd < 0)
        return;
    }
    conn_pop(c, len);
  }
}

int net_join(const char *ip)
{
  struct sockaddr_in a;
  int one = 1, fd;

  if (!core || state != NET_OFF)
    return -1;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(NET_PORT);
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
    return -1;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  set_nonblock(fd);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }
  memset(&conns[0], 0, sizeof(conns[0]));
  conns[0].fd = fd;
  snprintf(peer_ip, sizeof(peer_ip), "%s", ip);
  connect_start = now_ms();
  state = NET_CONNECTING;
  net_message("Joining...", 3000);
  return 0;
}

/* ---------------------------------------------------------------- both */

void net_set_interface(const struct retro_netpacket_callback *cb)
{
  int i;
  core = cb;
  for (i = 0; i < NET_MAX_CLIENTS; i++)
    if (!conns[i].in && !conns[i].out)
      conns[i].fd = -1;
}

int net_available(void) { return core != NULL; }

void net_set_game_name(const char *name)
{
  snprintf(game_name, sizeof(game_name), "%s", name);
}

void net_poll(void)
{
  /* the core may ask for packets from inside receive(): not again */
  if (in_poll)
    return;
  in_poll = 1;
  if (state == NET_HOSTING)
    host_poll();
  else if (state == NET_CONNECTING || state == NET_CONNECTED)
    client_poll();
  in_poll = 0;
}

void net_stop(void)
{
  int i;
  net_state_t was = state;

  if (was == NET_OFF)
    return;
  for (i = 0; i < NET_MAX_CLIENTS; i++)
    if (conns[i].fd >= 0)
      conn_close(&conns[i]);
  if (listen_fd >= 0)
    close(listen_fd);
  if (udp_fd >= 0)
    close(udp_fd);
  listen_fd = udp_fd = -1;
  state = NET_OFF;
  /* RetroArch: stop() when netplay ends (a client that never got in had
   * no start()) */
  if (was != NET_CONNECTING && core && core->stop)
    core->stop();
  net_message("Link closed", 2000);
}

net_state_t net_state(void) { return state; }

int net_clients(void)
{
  int i, n = 0;
  if (state != NET_HOSTING)
    return 0;
  for (i = 0; i < NET_MAX_CLIENTS; i++)
    if (conns[i].fd >= 0 && conns[i].id)
      n++;
  return n;
}

int net_client_id(void) { return my_id; }
const char *net_peer_ip(void) { return peer_ip; }

/* ---------------------------------------------------------------- discovery */

int net_discover(net_found_t *found, int max, unsigned timeout_ms)
{
  struct ifaddrs *ifs = NULL, *ifa;
  struct sockaddr_in to;
  unsigned long end;
  int fd, one = 1, n = 0;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return 0;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  set_nonblock(fd);

  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(NET_PORT);

  /* the broadcast address of each network (a WiFi access point of its own,
   * as Koriki's ADHOC, has no route for 255.255.255.255) */
  if (!getifaddrs(&ifs)) {
    for (ifa = ifs; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
        continue;
      if (ifa->ifa_flags & IFF_LOOPBACK) {
        /* (two consoles on one PC, for tests) */
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      } else if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr)
        to.sin_addr = ((struct sockaddr_in *)ifa->ifa_broadaddr)->sin_addr;
      else
        continue;
      sendto(fd, DISCOVER_ASK, sizeof(DISCOVER_ASK) - 1, 0, (struct sockaddr *)&to, sizeof(to));
    }
    freeifaddrs(ifs);
  }
  to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  sendto(fd, DISCOVER_ASK, sizeof(DISCOVER_ASK) - 1, 0, (struct sockaddr *)&to, sizeof(to));

  end = now_ms() + timeout_ms;
  while (n < max && now_ms() < end) {
    char buf[128];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    ssize_t len = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
    char ip[16], *bar;
    int i, dup = 0;

    if (len <= 0) {
      usleep(10000);
      continue;
    }
    buf[len] = 0;
    if (len < (ssize_t)sizeof(DISCOVER_ANS) - 1 || memcmp(buf, DISCOVER_ANS, sizeof(DISCOVER_ANS) - 1))
      continue;
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    for (i = 0; i < n; i++)
      if (!strcmp(found[i].ip, ip))
        dup = 1;
    if (dup)
      continue;
    bar = strchr(buf, '|');
    snprintf(found[n].ip, sizeof(found[n].ip), "%s", ip);
    found[n].players = atoi(buf + sizeof(DISCOVER_ANS) - 1);
    snprintf(found[n].name, sizeof(found[n].name), "%s", bar ? bar + 1 : "?");
    n++;
  }
  close(fd);
  return n;
}
