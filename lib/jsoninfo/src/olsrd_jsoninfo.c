
/*
 * The olsr.org Optimized Link-State Routing daemon(olsrd)
 * Copyright (c) 2004, Andreas Tonnesen(andreto@olsr.org)
 *                     includes code by Bruno Randolf
 *                     includes code by Andreas Lopatic
 *                     includes code by Sven-Ola Tuecke
 *                     includes code by Lorenz Schori
 *                     includes bugs by Markus Kittenberger
 *                     includes bugs by Hans-Christoph Steiner
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 */

/*
 * Dynamic linked library for the olsr.org olsr daemon
 */


#include <sys/types.h>
#include <sys/socket.h>
#ifndef _WIN32
#include <sys/select.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "ipcalc.h"
#include "olsr.h"
#include "olsr_types.h"
#include "neighbor_table.h"
#include "two_hop_neighbor_table.h"
#include "mpr_selector_set.h"
#include "tc_set.h"
#include "hna_set.h"
#include "mid_set.h"
#include "link_set.h"
#include "net_olsr.h"
#include "lq_plugin.h"
#include "common/autobuf.h"
#include "gateway.h"

#include "olsrd_jsoninfo.h"
#include "olsrd_plugin.h"

#ifdef _WIN32
#define close(x) closesocket(x)
#endif

static int ipc_socket;

/* IPC initialization function */
static int plugin_ipc_init(void);

static void abuf_json_open_array(struct autobuf *abuf, const char* header);
static void abuf_json_close_array(struct autobuf *abuf);
static void abuf_json_open_array_entry(struct autobuf *abuf);
static void abuf_json_close_array_entry(struct autobuf *abuf);
static void abuf_json_boolean(struct autobuf *abuf, const char* key, int value);
static void abuf_json_string(struct autobuf *abuf, const char* key, const char* value);
static void abuf_json_int(struct autobuf *abuf, const char* key, int value);
static void abuf_json_float(struct autobuf *abuf, const char* key, float value);

static void send_info(unsigned int /*send_what*/, int /*socket*/);
static void ipc_action(int, void *, unsigned int);
static void ipc_print_neighbors(struct autobuf *);
static void ipc_print_links(struct autobuf *);
static void ipc_print_routes(struct autobuf *);
static void ipc_print_topology(struct autobuf *);
static void ipc_print_hna(struct autobuf *);
static void ipc_print_mid(struct autobuf *);
static void ipc_print_gateways(struct autobuf *);
static void ipc_print_config(struct autobuf *);
static void ipc_print_interfaces(struct autobuf *);

#define TXT_IPC_BUFSIZE 256

#define SIW_NEIGHBORS 0x0001
#define SIW_LINKS 0x0002
#define SIW_ROUTES 0x0004
#define SIW_HNA 0x0008
#define SIW_MID 0x0010
#define SIW_TOPOLOGY 0x0020
#define SIW_GATEWAYS 0x0040
#define SIW_INTERFACES 0x0080
#define SIW_CONFIG 0x0100

/* ALL = neighbors links routes hna mid topology gateways interfaces */
#define SIW_ALL 0x00FF

#define MAX_CLIENTS 3

static char *outbuffer[MAX_CLIENTS];
static size_t outbuffer_size[MAX_CLIENTS];
static size_t outbuffer_written[MAX_CLIENTS];
static int outbuffer_socket[MAX_CLIENTS];
static int outbuffer_count;

static struct timer_entry *writetimer_entry;


/* JSON support functions */


/* JSON does not tolerate commas dangling at the end of arrays, so we need to
 * count which entry number we're at in order to make sure we don't tack a
 * dangling comma on at the end */
static int entrynumber = 0;
static int arrayentrynumber = 0;

static void
abuf_json_open_array(struct autobuf *abuf, const char* header)
{
  arrayentrynumber = 0;
  abuf_appendf(abuf, "{\"%s\": [\n", header);
}

static void
abuf_json_close_array(struct autobuf *abuf)
{
  abuf_appendf(abuf, "]}\n");
}

static void
abuf_json_open_array_entry(struct autobuf *abuf)
{
  entrynumber = 0;
  if (arrayentrynumber)
    abuf_appendf(abuf, ",\n{");
  else
    abuf_appendf(abuf, "{");
  arrayentrynumber++;
}

static void
abuf_json_close_array_entry(struct autobuf *abuf)
{
  abuf_appendf(abuf, "}");
}

static void
abuf_json_boolean(struct autobuf *abuf, const char* key, int value)
{
  if (entrynumber)
    abuf_appendf(abuf, ",\n");
  else
    abuf_appendf(abuf, "\n");
  abuf_appendf(abuf, "\t\"%s\": %s", key, value ? "true" : "false");
  entrynumber++;
}

static void
abuf_json_string(struct autobuf *abuf, const char* key, const char* value)
{
  if (entrynumber)
    abuf_appendf(abuf, ",\n");
  else
    abuf_appendf(abuf, "\n");
  abuf_appendf(abuf, "\t\"%s\": \"%s\"", key, value);
  entrynumber++;
}

static void
abuf_json_int(struct autobuf *abuf, const char* key, int value)
{
  if (entrynumber)
    abuf_appendf(abuf, ",\n");
  else
    abuf_appendf(abuf, "\n");
  abuf_appendf(abuf, "\t\"%s\": %i", key, value);
  entrynumber++;
}

static void
abuf_json_float(struct autobuf *abuf, const char* key, float value)
{
  if (entrynumber)
    abuf_appendf(abuf, ",\n");
  else
    abuf_appendf(abuf, "\n");
  abuf_appendf(abuf, "\t\"%s\": %.03f", key, value);
  entrynumber++;
}


/**
 *Do initialization here
 *
 *This function is called by the my_init
 *function in uolsrd_plugin.c
 */
int
olsrd_plugin_init(void)
{
  /* Initial IPC value */
  ipc_socket = -1;

  plugin_ipc_init();
  return 1;
}

/**
 * destructor - called at unload
 */
void
olsr_plugin_exit(void)
{
  if (ipc_socket != -1)
    close(ipc_socket);
}

static int
plugin_ipc_init(void)
{
  union olsr_sockaddr sst;
  uint32_t yes = 1;
  socklen_t addrlen;

  /* Init ipc socket */
  if ((ipc_socket = socket(olsr_cnf->ip_version, SOCK_STREAM, 0)) == -1) {
#ifndef NODEBUG
    olsr_printf(1, "(JSONINFO) socket()=%s\n", strerror(errno));
#endif
    return 0;
  } else {
    if (setsockopt(ipc_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes)) < 0) {
#ifndef NODEBUG
      olsr_printf(1, "(JSONINFO) setsockopt()=%s\n", strerror(errno));
#endif
      return 0;
    }
#if (defined __FreeBSD__ || defined __FreeBSD_kernel__) && defined SO_NOSIGPIPE
    if (setsockopt(ipc_socket, SOL_SOCKET, SO_NOSIGPIPE, (char *)&yes, sizeof(yes)) < 0) {
      perror("SO_REUSEADDR failed");
      return 0;
    }
#endif
    /* Bind the socket */

    /* complete the socket structure */
    memset(&sst, 0, sizeof(sst));
    if (olsr_cnf->ip_version == AF_INET) {
      sst.in4.sin_family = AF_INET;
      addrlen = sizeof(struct sockaddr_in);
#ifdef SIN6_LEN
      sst.in4.sin_len = addrlen;
#endif
      sst.in4.sin_addr.s_addr = jsoninfo_listen_ip.v4.s_addr;
      sst.in4.sin_port = htons(ipc_port);
    } else {
      sst.in6.sin6_family = AF_INET6;
      addrlen = sizeof(struct sockaddr_in6);
#ifdef SIN6_LEN
      sst.in6.sin6_len = addrlen;
#endif
      sst.in6.sin6_addr = jsoninfo_listen_ip.v6;
      sst.in6.sin6_port = htons(ipc_port);
    }

    /* bind the socket to the port number */
    if (bind(ipc_socket, &sst.in, addrlen) == -1) {
#ifndef NODEBUG
      olsr_printf(1, "(JSONINFO) bind()=%s\n", strerror(errno));
#endif
      return 0;
    }

    /* show that we are willing to listen */
    if (listen(ipc_socket, 1) == -1) {
#ifndef NODEBUG
      olsr_printf(1, "(JSONINFO) listen()=%s\n", strerror(errno));
#endif
      return 0;
    }

    /* Register with olsrd */
    add_olsr_socket(ipc_socket, &ipc_action, NULL, NULL, SP_PR_READ);

#ifndef NODEBUG
    olsr_printf(2, "(JSONINFO) listening on port %d\n", ipc_port);
#endif
  }
  return 1;
}

static void
ipc_action(int fd, void *data __attribute__ ((unused)), unsigned int flags __attribute__ ((unused)))
{
  union olsr_sockaddr pin;

  char addr[INET6_ADDRSTRLEN];
  fd_set rfds;
  struct timeval tv;
  unsigned int send_what = 0;
  int ipc_connection;

  socklen_t addrlen = sizeof(pin);

  if ((ipc_connection = accept(fd, &pin.in, &addrlen)) == -1) {
#ifndef NODEBUG
    olsr_printf(1, "(JSONINFO) accept()=%s\n", strerror(errno));
#endif
    return;
  }

  tv.tv_sec = tv.tv_usec = 0;
  if (olsr_cnf->ip_version == AF_INET) {
    if (inet_ntop(olsr_cnf->ip_version, &pin.in4.sin_addr, addr, INET6_ADDRSTRLEN) == NULL)
      addr[0] = '\0';
    if (!ip4equal(&pin.in4.sin_addr, &jsoninfo_accept_ip.v4) && jsoninfo_accept_ip.v4.s_addr != INADDR_ANY) {
#ifdef JSONINFO_ALLOW_LOCALHOST
      if (pin.in4.sin_addr.s_addr != INADDR_LOOPBACK) {
#endif
        olsr_printf(1, "(JSONINFO) From host(%s) not allowed!\n", addr);
        close(ipc_connection);
        return;
#ifdef JSONINFO_ALLOW_LOCALHOST
      }
#endif
    }
  } else {
    if (inet_ntop(olsr_cnf->ip_version, &pin.in6.sin6_addr, addr, INET6_ADDRSTRLEN) == NULL)
      addr[0] = '\0';
    /* Use in6addr_any (::) in olsr.conf to allow anybody. */
    if (!ip6equal(&in6addr_any, &jsoninfo_accept_ip.v6) && !ip6equal(&pin.in6.sin6_addr, &jsoninfo_accept_ip.v6)) {
      olsr_printf(1, "(JSONINFO) From host(%s) not allowed!\n", addr);
      close(ipc_connection);
      return;
    }
  }

#ifndef NODEBUG
  olsr_printf(2, "(JSONINFO) Connect from %s\n", addr);
#endif

  /* purge read buffer to prevent blocking on linux */
  FD_ZERO(&rfds);
  FD_SET((unsigned int)ipc_connection, &rfds);  /* Win32 needs the cast here */
  if (0 <= select(ipc_connection + 1, &rfds, NULL, NULL, &tv)) {
    char requ[128];
    ssize_t s = recv(ipc_connection, (void *)&requ, sizeof(requ), 0);   /* Win32 needs the cast here */
    if (0 < s) {
      requ[s] = 0;
      /* print out the requested tables */
      if (0 != strstr(requ, "/status")) send_what = SIW_ALL;
      else { /* included in /status */
        if (0 != strstr(requ, "/neighbors")) send_what |= SIW_NEIGHBORS;
        if (0 != strstr(requ, "/links")) send_what |= SIW_LINKS;
        if (0 != strstr(requ, "/routes")) send_what |= SIW_ROUTES;
        if (0 != strstr(requ, "/hna")) send_what |= SIW_HNA;
        if (0 != strstr(requ, "/mid")) send_what |= SIW_MID;
        if (0 != strstr(requ, "/topology")) send_what |= SIW_TOPOLOGY;
        if (0 != strstr(requ, "/gateways")) send_what |= SIW_GATEWAYS;
        if (0 != strstr(requ, "/interfaces")) send_what |= SIW_INTERFACES;
      }
      if (0 != strstr(requ, "/config")) send_what |= SIW_CONFIG;
    }
    if ( send_what == 0 ) send_what = SIW_ALL;
  }

  send_info(send_what, ipc_connection);
}

static void
ipc_print_neighbors(struct autobuf *abuf)
{
  struct ipaddr_str buf1;
  struct neighbor_entry *neigh;
  struct neighbor_2_list_entry *list_2;
  int thop_cnt;

  abuf_json_open_array(abuf, "neighbors");

  /* Neighbors */
  OLSR_FOR_ALL_NBR_ENTRIES(neigh) {
    abuf_json_open_array_entry(abuf);
    abuf_json_string(abuf, "ipv4Address",
                     olsr_ip_to_string(&buf1, &neigh->neighbor_main_addr));
    abuf_json_boolean(abuf, "symmetric", (neigh->status == SYM));
    abuf_json_boolean(abuf, "multiPointRelay", neigh->is_mpr);
    abuf_json_boolean(abuf, "multiPointRelaySelector",
                      olsr_lookup_mprs_set(&neigh->neighbor_main_addr) != NULL);
    abuf_json_int(abuf, "willingness", neigh->willingness);
    abuf_appendf(abuf, ",\n");

    thop_cnt = 0;
    if (neigh->neighbor_2_list.next) {
      abuf_appendf(abuf, "\t\"twoHopNeighbors\": [");
      for (list_2 = neigh->neighbor_2_list.next; list_2 != &neigh->neighbor_2_list; list_2 = list_2->next) {
        if (thop_cnt)
          abuf_appendf(abuf, ", ");
        abuf_appendf(abuf,
                     "\"%s\"",
                     olsr_ip_to_string(&buf1, &list_2->neighbor_2->neighbor_2_addr));
        thop_cnt++;
      }
    }
    abuf_appendf(abuf, "]");
    abuf_json_int(abuf, "twoHopNeighborCount", thop_cnt);
    abuf_json_close_array_entry(abuf);
  }
  OLSR_FOR_ALL_NBR_ENTRIES_END(neigh);
  abuf_json_close_array(abuf);
}

static void
ipc_print_links(struct autobuf *abuf)
{
  struct ipaddr_str buf1, buf2;
  struct lqtextbuffer lqbuffer1, lqbuffer2;

  struct link_entry *my_link = NULL;

  abuf_json_open_array(abuf, "links");
  OLSR_FOR_ALL_LINK_ENTRIES(my_link) {
    const char* lqs;
    int diff = (unsigned int)(my_link->link_timer->timer_clock - now_times);

    abuf_json_open_array_entry(abuf);
    abuf_json_string(abuf, "localIP",
                     olsr_ip_to_string(&buf1, &my_link->local_iface_addr));
    abuf_json_string(abuf, "remoteIP",
                     olsr_ip_to_string(&buf2, &my_link->neighbor_iface_addr));
    abuf_json_int(abuf, "msValid", diff);
    lqs = get_link_entry_text(my_link, '\t', &lqbuffer1);
    abuf_json_float(abuf, "linkQuality", atof(lqs));
    abuf_json_float(abuf, "neighborLinkQuality", atof(strrchr(lqs, '\t')));
    abuf_json_float(abuf, "linkCost",
                    atof(get_linkcost_text(my_link->linkcost, false, &lqbuffer2)));
    abuf_json_close_array_entry(abuf);
  }
  OLSR_FOR_ALL_LINK_ENTRIES_END(my_link);
  abuf_json_close_array(abuf);
}

static void
ipc_print_routes(struct autobuf *abuf)
{
  struct ipaddr_str buf1, buf2;
  struct rt_entry *rt;
  struct lqtextbuffer lqbuffer;

  //abuf_puts(abuf, "Table: Routes\nDestination\tGateway IP\tMetric\tETX\tInterface\n");
  abuf_json_open_array(abuf, "routes");

  /* Walk the route table */
  OLSR_FOR_ALL_RT_ENTRIES(rt) {
    abuf_json_open_array_entry(abuf);
    abuf_json_string(abuf, "destination",
                     olsr_ip_to_string(&buf1, &rt->rt_dst.prefix));
    abuf_json_int(abuf, "genmask", rt->rt_dst.prefix_len);
    abuf_json_string(abuf, "gateway",
                     olsr_ip_to_string(&buf2, &rt->rt_best->rtp_nexthop.gateway));
    abuf_json_int(abuf, "metric", rt->rt_best->rtp_metric.hops);
    abuf_json_float(abuf, "expectedTransmissionCount",
                    atof(get_linkcost_text(rt->rt_best->rtp_metric.cost, true, &lqbuffer)));
    abuf_json_string(abuf, "interface",
                     if_ifwithindex_name(rt->rt_best->rtp_nexthop.iif_index));
    abuf_json_close_array_entry(abuf);
  }
  OLSR_FOR_ALL_RT_ENTRIES_END(rt);

  abuf_json_close_array(abuf);
}

static void
ipc_print_topology(struct autobuf *abuf)
{
  struct tc_entry *tc;

  abuf_json_open_array(abuf, "topology");
  //abuf_puts(abuf, "Table: Topology\nDest. IP\tLast hop IP\tLQ\tNLQ\tCost\tVTime\n");

  /* Topology */
  OLSR_FOR_ALL_TC_ENTRIES(tc) {
    struct tc_edge_entry *tc_edge;
    OLSR_FOR_ALL_TC_EDGE_ENTRIES(tc, tc_edge) {
      if (tc_edge->edge_inv) {
        struct ipaddr_str dstbuf, addrbuf;
        struct lqtextbuffer lqbuffer1, lqbuffer2;
        uint32_t vt = tc->validity_timer != NULL ? (tc->validity_timer->timer_clock - now_times) : 0;
        int diff = (int)(vt);
        const char* lqs;
        abuf_json_open_array_entry(abuf);
        abuf_json_string(abuf, "destinationIP",
                         olsr_ip_to_string(&dstbuf, &tc_edge->T_dest_addr));
        abuf_json_string(abuf, "lastHopIP",
                         olsr_ip_to_string(&addrbuf, &tc->addr));
        lqs = get_tc_edge_entry_text(tc_edge, '\t', &lqbuffer1);
        abuf_json_float(abuf, "linkQuality", atof(lqs));
        abuf_json_float(abuf, "neighborLinkQuality", atof(strrchr(lqs, '\t')));
        abuf_json_float(abuf, "cost", 
                        atof(get_linkcost_text(tc_edge->cost, false, &lqbuffer2)));
        abuf_json_int(abuf, "msValid", diff);
        abuf_json_close_array_entry(abuf);
      }
    }
    OLSR_FOR_ALL_TC_EDGE_ENTRIES_END(tc, tc_edge);
  }
  OLSR_FOR_ALL_TC_ENTRIES_END(tc);

  abuf_json_close_array(abuf);
}

static void
ipc_print_hna(struct autobuf *abuf)
{
  struct ip_prefix_list *hna;
  struct hna_entry *tmp_hna;
  struct hna_net *tmp_net;
  struct ipaddr_str buf, mainaddrbuf;

  abuf_json_open_array(abuf, "hna");
  //abuf_puts(abuf, "Table: HNA\nDestination\tGateway\tVTime\n");

  /* Announced HNA entries */
  if (olsr_cnf->ip_version == AF_INET) {
    for (hna = olsr_cnf->hna_entries; hna != NULL; hna = hna->next) {
      abuf_appendf(abuf,
                   "%s/%d\t%s\n",
                   olsr_ip_to_string(&buf, &hna->net.prefix),
                   hna->net.prefix_len,
                   olsr_ip_to_string(&mainaddrbuf, &olsr_cnf->main_addr));
    }
  } else {
    for (hna = olsr_cnf->hna_entries; hna != NULL; hna = hna->next) {
      abuf_appendf(abuf,
                   "%s/%d\t%s\n",
                   olsr_ip_to_string(&buf, &hna->net.prefix),
                   hna->net.prefix_len,
                   olsr_ip_to_string(&mainaddrbuf, &olsr_cnf->main_addr));
    }
  }

  /* HNA entries */
  OLSR_FOR_ALL_HNA_ENTRIES(tmp_hna) {

    /* Check all networks */
    for (tmp_net = tmp_hna->networks.next; tmp_net != &tmp_hna->networks; tmp_net = tmp_net->next) {
      uint32_t vt = tmp_net->hna_net_timer != NULL ? (tmp_net->hna_net_timer->timer_clock - now_times) : 0;
      int diff = (int)(vt);
      abuf_appendf(abuf,
                   "%s/%d\t%s\t\%d.%03d\n",
                   olsr_ip_to_string(&buf, &tmp_net->hna_prefix.prefix),
                   tmp_net->hna_prefix.prefix_len,
                   olsr_ip_to_string(&mainaddrbuf, &tmp_hna->A_gateway_addr),
                   diff/1000, abs(diff%1000));
    }
  }
  OLSR_FOR_ALL_HNA_ENTRIES_END(tmp_hna);

  abuf_json_close_array(abuf);
}

static void
ipc_print_mid(struct autobuf *abuf)
{
  int idx;
  unsigned short is_first;
  struct mid_entry *entry;
  struct mid_address *alias;

  abuf_json_open_array(abuf, "mid");
  //abuf_puts(abuf, "Table: MID\nIP address\tAlias\tVTime\n");

  /* MID */
  for (idx = 0; idx < HASHSIZE; idx++) {
    entry = mid_set[idx].next;

    while (entry != &mid_set[idx]) {
      struct ipaddr_str buf, buf2;
      alias = entry->aliases;
      is_first = 1;

      while (alias) {
        uint32_t vt = alias->vtime - now_times;
        int diff = (int)(vt);

        abuf_appendf(abuf, "%s\t%s\t%d.%03d\n",
                     olsr_ip_to_string(&buf, &entry->main_addr),
                     olsr_ip_to_string(&buf2, &alias->alias),
                     diff/1000, abs(diff%1000));
        alias = alias->next_alias;
        is_first = 0;
      }
      entry = entry->next;
    }
  }
  abuf_json_close_array(abuf);
}

static void
ipc_print_gateways(struct autobuf *abuf)
{
#ifndef linux
  abuf_json_string(abuf, "error", "Gateway mode is only supported in Linux");
#else
  static const char IPV4[] = "ipv4";
  static const char IPV4_NAT[] = "ipv4(n)";
  static const char IPV6[] = "ipv6";
  static const char NONE[] = "-";

  struct ipaddr_str buf;
  struct gateway_entry *gw;
  struct lqtextbuffer lqbuf;

  // Status IP ETX Hopcount Uplink-Speed Downlink-Speed ipv4/ipv4-nat/- ipv6/- ipv6-prefix/-
  abuf_json_open_array(abuf, "gateways");
  //abuf_puts(abuf, "Table: Gateways\nStatus\tGateway IP\tETX\tHopcnt\tUplink\tDownlnk\tIPv4\tIPv6\tPrefix\n");
  OLSR_FOR_ALL_GATEWAY_ENTRIES(gw) {
    char v4 = '-', v6 = '-';
    bool autoV4 = false, autoV6 = false;
    const char *v4type = NONE, *v6type = NONE;
    struct tc_entry *tc;

    if ((tc = olsr_lookup_tc_entry(&gw->originator)) == NULL) {
      continue;
    }

    if (gw == olsr_get_ipv4_inet_gateway(&autoV4)) {
      v4 = autoV4 ? 'a' : 's';
    } else if (gw->ipv4 && (olsr_cnf->ip_version == AF_INET || olsr_cnf->use_niit)
               && (olsr_cnf->smart_gw_allow_nat || !gw->ipv4nat)) {
      v4 = 'u';
    }

    if (gw == olsr_get_ipv6_inet_gateway(&autoV6)) {
      v6 = autoV6 ? 'a' : 's';
    } else if (gw->ipv6 && olsr_cnf->ip_version == AF_INET6) {
      v6 = 'u';
    }

    if (gw->ipv4) {
      v4type = gw->ipv4nat ? IPV4_NAT : IPV4;
    }
    if (gw->ipv6) {
      v6type = IPV6;
    }

    abuf_appendf(abuf, "%c%c\t%s\t%s\t%d\t%u\t%u\t%s\t%s\t%s\n",
                 v4, v6, olsr_ip_to_string(&buf, &gw->originator),
                 get_linkcost_text(tc->path_cost, true, &lqbuf), tc->hops,
                 gw->uplink, gw->downlink, v4type, v6type,
                 gw->external_prefix.prefix_len == 0 ? NONE : olsr_ip_prefix_to_string(&gw->external_prefix));
  }
  OLSR_FOR_ALL_GATEWAY_ENTRIES_END(gw)
  abuf_json_close_array(abuf);
#endif
}

static void
ipc_print_config(struct autobuf *abuf)
{
  olsrd_write_cnf_autobuf(abuf, olsr_cnf);
}

static void
ipc_print_interfaces(struct autobuf *abuf)
{
  const struct olsr_if *ifs;
  abuf_json_open_array(abuf, "interfaces");
  //abuf_puts(abuf, "Table: Interfaces\nName\tState\tMTU\tWLAN\tSrc-Adress\tMask\tDst-Adress\n");
  for (ifs = olsr_cnf->interfaces; ifs != NULL; ifs = ifs->next) {
    const struct interface *const rifs = ifs->interf;
    abuf_json_open_array_entry(abuf);
    abuf_json_string(abuf, "name", ifs->name);
    if (!rifs) {
      abuf_json_string(abuf, "state", "down");
    } else {
      abuf_json_string(abuf, "state", "up");
      abuf_json_int(abuf, "mtu", rifs->int_mtu);
      abuf_json_boolean(abuf, "wireless", rifs->is_wireless);

      if (olsr_cnf->ip_version == AF_INET) {
        struct ipaddr_str addrbuf, maskbuf, bcastbuf;
        abuf_json_string(abuf, "ipv4Address",
                             ip4_to_string(&addrbuf, rifs->int_addr.sin_addr));
        abuf_json_string(abuf, "netmask",
                             ip4_to_string(&maskbuf, rifs->int_netmask.sin_addr));
        abuf_json_string(abuf, "broadcast",
                             ip4_to_string(&bcastbuf, rifs->int_broadaddr.sin_addr));
      } else {
        struct ipaddr_str addrbuf, maskbuf;
        abuf_json_string(abuf, "ipv6Address",
                             ip6_to_string(&addrbuf, &rifs->int6_addr.sin6_addr));
        abuf_json_string(abuf, "multicast",
                             ip6_to_string(&maskbuf, &rifs->int6_multaddr.sin6_addr));
      }
    }
    abuf_json_close_array_entry(abuf);
  }
  abuf_json_close_array(abuf);
}


static void
jsoninfo_write_data(void *foo __attribute__ ((unused)))
{
  fd_set set;
  int result, i, j, max;
  struct timeval tv;

  FD_ZERO(&set);
  max = 0;
  for (i=0; i<outbuffer_count; i++) {
    /* And we cast here since we get a warning on Win32 */
    FD_SET((unsigned int)(outbuffer_socket[i]), &set);

    if (outbuffer_socket[i] > max) {
      max = outbuffer_socket[i];
    }
  }

  tv.tv_sec = 0;
  tv.tv_usec = 0;

  result = select(max + 1, NULL, &set, NULL, &tv);
  if (result <= 0) {
    return;
  }

  for (i=0; i<outbuffer_count; i++) {
    if (FD_ISSET(outbuffer_socket[i], &set)) {
      result = send(outbuffer_socket[i],
                    outbuffer[i] + outbuffer_written[i],
                    outbuffer_size[i] - outbuffer_written[i],
                    0);
      if (result > 0) {
        outbuffer_written[i] += result;
      }

      if (result <= 0 || outbuffer_written[i] == outbuffer_size[i]) {
        /* close this socket and cleanup*/
        close(outbuffer_socket[i]);
        free (outbuffer[i]);

        for (j=i+1; j<outbuffer_count; j++) {
          outbuffer[j-1] = outbuffer[j];
          outbuffer_size[j-1] = outbuffer_size[j];
          outbuffer_socket[j-1] = outbuffer_socket[j];
          outbuffer_written[j-1] = outbuffer_written[j];
        }
        outbuffer_count--;
      }
    }
  }
  if (outbuffer_count == 0) {
    olsr_stop_timer(writetimer_entry);
  }
}

static void
send_info(unsigned int send_what, int the_socket)
{
  struct autobuf abuf;

  abuf_init(&abuf, 4096);

  /* Print minimal http header */
  abuf_puts(&abuf, "HTTP/1.0 200 OK\n");
  abuf_puts(&abuf, "Content-type: text/plain\n\n");

  /* Print tables to IPC socket */

  if ((send_what & SIW_LINKS) == SIW_LINKS)
    ipc_print_links(&abuf);
  if ((send_what & SIW_NEIGHBORS) == SIW_NEIGHBORS)
    ipc_print_neighbors(&abuf);
  if ((send_what & SIW_TOPOLOGY) == SIW_TOPOLOGY)
    ipc_print_topology(&abuf);
  if ((send_what & SIW_HNA) == SIW_HNA)
    ipc_print_hna(&abuf);
  if ((send_what & SIW_MID) == SIW_MID)
    ipc_print_mid(&abuf);
  if ((send_what & SIW_ROUTES) == SIW_ROUTES)
    ipc_print_routes(&abuf);
  if ((send_what & SIW_GATEWAYS) == SIW_GATEWAYS)
    ipc_print_gateways(&abuf);
  if ((send_what & SIW_CONFIG) == SIW_CONFIG)
    ipc_print_config(&abuf);
  if ((send_what & SIW_INTERFACES) == SIW_INTERFACES)
    ipc_print_interfaces(&abuf);

  outbuffer[outbuffer_count] = olsr_malloc(abuf.len, "txt output buffer");
  outbuffer_size[outbuffer_count] = abuf.len;
  outbuffer_written[outbuffer_count] = 0;
  outbuffer_socket[outbuffer_count] = the_socket;

  memcpy(outbuffer[outbuffer_count], abuf.buf, abuf.len);
  outbuffer_count++;

  if (outbuffer_count == 1) {
    writetimer_entry = olsr_start_timer(100,
                                        0,
                                        OLSR_TIMER_PERIODIC,
                                        &jsoninfo_write_data,
                                        NULL,
                                        0);
  }

  abuf_free(&abuf);
}

/*
 * Local Variables:
 * mode: c
 * style: linux
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * End:
 */