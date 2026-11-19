#ifndef FRONTEND_NET_H
#define FRONTEND_NET_H

#include <stdint.h>
#include "libretro.h"

/* Netplay for the core's netpacket interface (Cable Link, Wireless Adapter,
 * Pokemon / Advance Wars modes) between standalone consoles on a local
 * network. It does what RetroArch's netplay does for a netpacket core: it
 * only carries the core's packets (no input sync, no rollback), the clients
 * talk to the host and the host passes on what is meant for the others. */

#define NET_PORT        55435   /* same as RetroArch */
#define NET_MAX_CLIENTS 3       /* the cable has 4 ends */
#define NET_MAX_FOUND   8

typedef enum {
  NET_OFF = 0,
  NET_HOSTING,
  NET_CONNECTING,     /* client: TCP connect / waiting for the welcome */
  NET_CONNECTED,      /* client: in the session */
} net_state_t;

typedef struct {
  char ip[16];
  char name[48];      /* game the host runs */
  int players;        /* consoles in the session, host included */
} net_found_t;

/* the core's interface (RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE) */
void net_set_interface(const struct retro_netpacket_callback *cb);
int net_available(void);

/* game name sent to the consoles looking for a host */
void net_set_game_name(const char *name);

int net_host(void);                 /* 0 = hosting */
int net_join(const char *ip);       /* 0 = connecting */
void net_stop(void);                /* leave / close the session */

/* once per frame, and whenever the core asks for packets */
void net_poll(void);

net_state_t net_state(void);
int net_clients(void);              /* host: clients in the session */
int net_client_id(void);            /* client: our number (1..3) */
const char *net_peer_ip(void);      /* client: the host's address */

/* asks the local networks for hosts, waiting up to timeout_ms */
int net_discover(net_found_t *found, int max, unsigned timeout_ms);

/* the frontend shows these on screen */
void net_message(const char *text, unsigned ms);

#endif
