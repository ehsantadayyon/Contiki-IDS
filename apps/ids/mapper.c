/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"
#include "net/uip.h"
#include "net/rpl/rpl.h"
#include "net/rime/rimeaddr.h"

#include "ids-central.h"

#include "net/netstack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEBUG DEBUG_PRINT
#include "net/uip-debug.h"

static struct uip_udp_conn *ids_conn;
static int working_host = 0;
static uint8_t current_rpl_instance_id;
static uip_ipaddr_t current_dag_id;
static int mapper_instance = 0;
static int mapper_dag = 0;
static struct etimer map_timer;

PROCESS(mapper, "IDS network mapper");
AUTOSTART_PROCESSES(&mapper);

struct Node {
  uip_ipaddr_t * id;
  struct Node * parent;
  struct Node * children[NETWORK_NODES/4];
  int child;
};

static struct Node network[NETWORK_NODES];
static int node_index = 0;

/**
 * Find the node specified or initialize a new one if it does not already exist
 *
 *
 * @return Returns NULL if we run out of memory
 */
struct Node * find_node(uip_ipaddr_t * ip) {
  int i;
  for (i = 0; i < node_index; ++i) {
    if (uip_ipaddr_cmp(ip, network[i].id)) {
      return &network[i];
    }
  }
  return NULL;
}

/**
 * Add a new node to the network graph
 *
 * Note that as the ip pointer is saved the memory it points to needs to be
 * kept
 *
 * If the node already exists in the network no new node will be added and a
 * pointer to that adress will be returned
 *
 * @return A pointer to the new node or NULL if we ran out of memory
 */
struct Node * add_node(uip_ipaddr_t * ip) {
  if (node_index >= NETWORK_NODES-1) // Out of memory
    return NULL;

  struct Node * node = find_node(ip);

  if (node != NULL)
    return node;

  printf("Creating new node: ");
  uip_debug_ipaddr_print(ip);
  printf("\n");
  network[node_index].id = ip;
  return &network[node_index++];
}

void print_subtree(struct Node * node, int depth) {
  int i;
  printf("%*s", depth*2, "");
  if (depth > 8) { // uggly hack to avoid loops
    printf("...\n");
    return;
  }

  uip_debug_ipaddr_print(node->id);
  printf("\n");
  for (i = 0; i < node->child; ++i) print_subtree(node->children[i], depth+1);
}

void print_graph() {
  printf("Network graph:\n\n");
  print_subtree(&network[0], 0);
  printf("-----------------------\n");
}

void tcpip_handler() {
  if (!uip_newdata())
    return;

  uint8_t *appdata;
  uip_ipaddr_t src_ip, parent_ip;
  struct Node *id, *parent;

  appdata = (uint8_t *)uip_appdata;
  memcpy(&src_ip, appdata, sizeof(src_ip));
  printf("Source IP: ");
  uip_debug_ipaddr_print(&src_ip);
  src_ip.u16[0] = 0xaaaa;
  printf("\n");
  id = find_node(&src_ip);
  if (id == NULL)
    return;
  printf("FOund node\n");

  appdata += sizeof(uint8_t) + 2*sizeof(uip_ipaddr_t);
  memcpy(&parent_ip, appdata, sizeof(parent_ip));
  parent_ip.u16[0] = 0xaaaa;
  parent = find_node(&parent_ip);
  if (parent == NULL)
    return;
  printf("Found parent\n");

  int i;
  for (i = 0; i < parent->child; ++i) {
    if (uip_ipaddr_cmp(parent->children[i]->id, id->id))
      return;
  }

  id->parent = parent;
  parent->children[parent->child++] = id;

  printf("parent: ");
  uip_debug_ipaddr_print(&parent_ip);
  printf("\n");

  print_graph();

}

void map_network() {
  for (; working_host < UIP_DS6_ROUTE_NB && !uip_ds6_routing_table[working_host].isused; ++working_host);
  if (!uip_ds6_routing_table[working_host].isused) {
    ++working_host;
    return;
  }
  static char data[sizeof(current_rpl_instance_id) + sizeof(current_dag_id)];
  void * data_p = data;
  memcpy(data_p, &current_rpl_instance_id, sizeof(current_rpl_instance_id));
  data_p += sizeof(current_rpl_instance_id);
  memcpy(data_p, &current_dag_id, sizeof(current_dag_id));
  printf("sending data to ");
  uip_debug_ipaddr_print(&uip_ds6_routing_table[working_host].ipaddr);
  printf("\n");
  add_node(&uip_ds6_routing_table[working_host].ipaddr);
  uip_udp_packet_sendto(ids_conn, data, sizeof(data), &uip_ds6_routing_table[working_host++].ipaddr, UIP_HTONS(MAPPER_CLIENT_PORT));
  if (working_host >= UIP_DS6_ROUTE_NB-1) {
    working_host = 0;
    etimer_reset(&map_timer);
  }
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mapper, ev, data)
{
  static struct etimer timer;

  PROCESS_BEGIN();

  PROCESS_PAUSE();

  memset(host, 0, sizeof(host));
  memset(network, 0, sizeof(network));

  PRINTF("IDS Server, compile time: %s\n", __TIME__);

  ids_conn = udp_new(NULL, UIP_HTONS(MAPPER_CLIENT_PORT), NULL);
  udp_bind(ids_conn, UIP_HTONS(MAPPER_SERVER_PORT));

  PRINTF("Created a server connection with remote address ");
  PRINT6ADDR(&ids_conn->ripaddr);
  PRINTF(" local/remote port %u/%u\n", UIP_HTONS(ids_conn->lport),
      UIP_HTONS(ids_conn->rport));

  etimer_set(&timer, CLOCK_SECOND);
  etimer_set(&map_timer, 10*CLOCK_SECOND);

  // Add this node (root node) to the network graph
  // network[0].id = &uip_ds6_get_link_local(ADDR_PREFERRED)->ipaddr;
  network[0].id = &uip_ds6_get_global(ADDR_PREFERRED)->ipaddr;
  ++node_index;

  while(1) {
    printf("snurrar\n");
    PROCESS_YIELD();
    if(ev == tcpip_event) {
      tcpip_handler();
    }
    else if (etimer_expired(&map_timer)) { // && etimer_expired(&timer)) {
      // Map the next DAG.
      if (working_host == 0) {
        for (; mapper_instance < RPL_MAX_INSTANCES; ++mapper_instance) {
          if (instance_table[mapper_instance].used == 0)
            continue;
          for (; mapper_dag < RPL_MAX_DAG_PER_INSTANCE; ++mapper_dag) {
            if (!instance_table[mapper_instance].dag_table[mapper_dag].used)
              continue;

            current_rpl_instance_id = instance_table[mapper_instance].instance_id;
            memcpy(&current_dag_id, &instance_table[mapper_instance].dag_table[mapper_dag].dag_id, sizeof(current_dag_id));
            goto found_network;

          }
          if (mapper_dag >= RPL_MAX_DAG_PER_INSTANCE-1)
            mapper_dag = 0;
        }
        if (mapper_instance >= RPL_MAX_INSTANCES-1)
          mapper_instance = 0;
      }

found_network:
      map_network();
      etimer_reset(&timer);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/