//TODO: Split horizon implement
/* Filename: dr_api.c */

/* include files */
#include <arpa/inet.h>  /* htons, ... */
#include <sys/socket.h> /* AF_INET */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "dr_api.h"
#include "rmutex.h"

/* internal data structures */
#define INFINITY 16

#define RIP_IP htonl(0xE0000009)

#define RIP_COMMAND_REQUEST  1
#define RIP_COMMAND_RESPONSE 2
#define RIP_VERSION          2

#define RIP_ADVERT_INTERVAL_SEC 10
#define RIP_TIMEOUT_SEC 20
#define RIP_GARBAGE_SEC 20

#define IPV4_ADDR_FAM 1 //NOTE: Not sure if needed
#define DEBUG 1

/** information about a route which is sent with a RIP packet */
typedef struct rip_entry_t {
    uint16_t addr_family;
    uint16_t pad;
    uint32_t ip;
    uint32_t subnet_mask;
    uint32_t next_hop;
    uint32_t metric;
    uint32_t learned_from;
} __attribute__ ((packed)) rip_entry_t;

/** the RIP payload header */
typedef struct rip_header_t {
    char        command;
    char        version;
    uint16_t    pad;
    rip_entry_t entries[0];
} __attribute__ ((packed)) rip_header_t;

/** a single entry in the routing table */
typedef struct route_t {
    uint32_t subnet;        /* destination subnet which this route is for */
    uint32_t mask;          /* mask associated with this route */
    uint32_t next_hop_ip;   /* next hop on on this route */
    uint32_t outgoing_intf; /* interface to use to send packets on this route */
    uint32_t cost;
    uint32_t learned_from;
    struct timeval last_updated;

    int is_garbage; /* boolean which notes whether this entry is garbage */

    route_t* next;  /* pointer to the next route in a linked-list */
} route_t;


/* internal variables */

/* a very coarse recursive mutex to synchronize access to methods */
static rmutex_t coarse_lock;

/** how mlong to sleep between periodic callbacks */
static unsigned secs_to_sleep_between_callbacks;
static unsigned nanosecs_to_sleep_between_callbacks;


/* these static functions are defined by the dr */

/*** Returns the number of interfaces on the host we're currently connected to.*/
static unsigned (*dr_interface_count)();

/*** Returns a copy of the requested interface.  All fields will be 0 if the an* invalid interface index is requested.*/
static lvns_interface_t (*dr_get_interface)(unsigned index);

/*** Sends specified dynamic routing payload.** @param dst_ip   The ultimate destination of the packet.
 ** @param next_hop_ip  The IP of the next hop (either a router or the final dst).** @param outgoing_intf  Index of the interface to send the packet from.
 ** @param payload  This will be sent as the payload of the DR packet.  The caller*                 is reponsible for managing the memory associated with buf*                 (e.g. this function will NOT free buf).
 ** @param len      The number of bytes in the DR payload.*/
static void (*dr_send_payload)(uint32_t dst_ip,
                               uint32_t next_hop_ip,
                               uint32_t outgoing_intf,
                               char* /* borrowed */,
                               unsigned);


/* internal functions */
long get_time();
void print_ip(int ip);
void print_routing_table(route_t *head);
/* internal lock-safe methods for the students to implement */
struct timeval get_struct_timeval();
void append(route_t *head, route_t *new_entry);
void remove(route_t *to_remove);
uint32_t count_route_table_entries();
void print_packet(rip_entry_t *packet);
static next_hop_t safe_dr_get_next_hop(uint32_t ip);
void advertise_routing_table();
void broadcast_single_entry(route_t *);
void broadcast_intf_down(uint32_t );
static void safe_dr_handle_packet(uint32_t ip, unsigned intf,
                                  char* buf /* borrowed */, unsigned len);
static void safe_dr_handle_periodic();
static void safe_dr_interface_changed(unsigned intf,
                                      int state_changed,
                                      int cost_changed);

/*** This simple method is the entry point to a thread which will periodically* make a callback to your dr_handle_periodic method.*/
static void* periodic_callback_manager_main(void* nil) {
    struct timespec timeout;

    timeout.tv_sec = secs_to_sleep_between_callbacks;
    timeout.tv_nsec = nanosecs_to_sleep_between_callbacks;
    while(1) {
        nanosleep(&timeout, NULL);
        dr_handle_periodic();
    }

    return NULL;
}

next_hop_t dr_get_next_hop(uint32_t ip) {
    next_hop_t hop;
    rmutex_lock(&coarse_lock);
    hop = safe_dr_get_next_hop(ip);
    rmutex_unlock(&coarse_lock);
    return hop;
}

void dr_handle_packet(uint32_t ip, unsigned intf, char* buf /* borrowed */, unsigned len) {
    rmutex_lock(&coarse_lock);
    safe_dr_handle_packet(ip, intf, buf, len);
    rmutex_unlock(&coarse_lock);
}

void dr_handle_periodic() {
    rmutex_lock(&coarse_lock);
    safe_dr_handle_periodic();
    rmutex_unlock(&coarse_lock);
}

void dr_interface_changed(unsigned intf, int state_changed, int cost_changed) {
    rmutex_lock(&coarse_lock);
    safe_dr_interface_changed(intf, state_changed, cost_changed);
    rmutex_unlock(&coarse_lock);
}


route_t *head_rt = NULL; //Head of the routing table

void dr_init(unsigned (*func_dr_interface_count)(),
             lvns_interface_t (*func_dr_get_interface)(unsigned index),
             void (*func_dr_send_payload)(uint32_t dst_ip,
                                          uint32_t next_hop_ip,
                                          uint32_t outgoing_intf,
                                          char* /* borrowed */,
                                          unsigned)) {
    pthread_t tid;

    /* save the functions the DR is providing for us */
    dr_interface_count = func_dr_interface_count;
    dr_get_interface = func_dr_get_interface;
    dr_send_payload = func_dr_send_payload;

    /* initialize the recursive mutex */
    rmutex_init(&coarse_lock);

    /* initialize the amount of time we want between callbacks */
    secs_to_sleep_between_callbacks = 1;
    nanosecs_to_sleep_between_callbacks = 0;

    /* start a new thread to provide the periodic callbacks */
    if(pthread_create(&tid, NULL, periodic_callback_manager_main, NULL) != 0) {
        fprintf(stderr, "pthread_create failed in dr_initn");
        exit(1);
    }

    head_rt = (route_t *) malloc(sizeof(route_t));
    lvns_interface_t tmp;

    for(uint32_t i=0;i<dr_interface_count();i++){
      tmp = dr_get_interface(i);
      //if (DEBUG) print_ip(tmp.ip);
      route_t *new_entry = (route_t *) malloc(sizeof(route_t)); //DEBUG:Add catch of false malloc
      new_entry->subnet = tmp.subnet_mask & tmp.ip; //Destination
      new_entry->mask = tmp.subnet_mask;
      new_entry->next_hop_ip = 0; //NOTE: Not needed for initial, direct connections
      new_entry->outgoing_intf = i;
      new_entry->cost = tmp.cost;
      new_entry->last_updated = get_struct_timeval();
      new_entry->learned_from = 0;
      new_entry->is_garbage = 0;
      new_entry->next = NULL;

      if(i==0){
        head_rt = new_entry;
      } else{
        append(head_rt, new_entry);
      }
    }
    if(DEBUG) print_routing_table(head_rt);
}

next_hop_t safe_dr_get_next_hop(uint32_t ip) {
    next_hop_t hop;

    hop.interface = 0;
    hop.dst_ip = 0;

    /* determine the next hop in order to get to ip */
    route_t *current = head_rt;
    while(current != NULL){
      if((ip & current->mask) == current->subnet/* && current->cost < 16*/){
        hop.interface = current->outgoing_intf;
        hop.dst_ip = current->next_hop_ip;
        return hop; //There is only one entry to a certain IP/subnet
      }
      current = current->next;
    }
    hop.dst_ip = 0xFFFFFFFF;
    return hop;
}


void safe_dr_handle_packet(uint32_t ip, unsigned intf,
                           char* buf /* borrowed */, unsigned len) {
    /* handle the dynamic routing payload in the buf buffer */
    //uint8_t offset = 0;
    rip_entry_t *received = (rip_entry_t *) malloc(sizeof(rip_entry_t));
    rip_header_t *header = (rip_header_t *) malloc(sizeof(rip_header_t));

    memcpy(header, buf, sizeof(rip_header_t));
    memcpy(received, buf + sizeof(rip_header_t), sizeof(rip_entry_t));

    bool here_u_exists = false;
    bool here_v_exists = false;
    bool v_same_as_here = false;
    uint32_t v = received->ip;
    int32_t u_interface_index = -1;
    route_t *current = head_rt;
    route_t *here_u;
    route_t *here_v;


    for(uint32_t i=0;i<dr_interface_count();i++){
      lvns_interface_t tmp = dr_get_interface(i);
      if(received->learned_from == tmp.ip){
        fprintf(stderr, "%s\n", "Omit route!"); //This route has been learned from this IP and is now being send here again -> omit (Split horizon w/ poison reverse)
        received->metric = INFINITY;
      }
    }


    if(received->ip == received->next_hop){ /*The interface received->ip is down*/
      //fprintf(stderr, "%s ","Interface down with IP: ");
      //print_ip(received->ip);
      while(current != NULL){
        if(current->next_hop_ip == received->ip || current->subnet == received->ip){
          current->cost = INFINITY;
          broadcast_single_entry(current);
          broadcast_intf_down(received->ip);
          remove(current);
        }
        current = current->next;
      }
      return;
    }
    /*Received a connection (u --> v) with a cost c(u,v), where u is the router it came from
    and v is another router or subnet.
    First: Check in the RT, if we have an entry where subnet == u. If yes, update the timestamp
    if not, make a new entry with the correct intfc, associated cost, and next_hop = 0.
    Second: For the (u,v) we have received, check if (Here, u) exists (must bcs. of First).
    If it exists, check if (Here, v) exists.
      If NO: Add a new entry (Here, v) with next_hop
        equal u with metric c(Here, u) + c(u,v).
      If YES: Compare c(Here,v) >? c(Here, u) + c(u,v)
          if we have found a better route, update the metric to c(Here, u) + c(u,v) */
    /*Check if v == here*/
    for(uint32_t i=0;i<dr_interface_count();i++){
      lvns_interface_t tmp = dr_get_interface(i);
      if(v == tmp.ip){
        v_same_as_here = true;
      }
    }

    while(current != NULL){
      uint32_t end = current->subnet;
      if(end == ip){ //Is the endpoint of the entry the same as the IP that we are receiving this message from?
        here_u_exists = true;
        current->last_updated = get_struct_timeval(); //Reset the timestamp
        here_u = current;
        //fprintf(stderr, "%s\n", "Found Here -> u");
        /*Search the correct interface index*/
        for(uint32_t i=0;i<dr_interface_count();i++){
          lvns_interface_t tmp = dr_get_interface(i);
          if( (tmp.ip & tmp.subnet_mask)  == (here_u->subnet & tmp.subnet_mask) && tmp.enabled){
            u_interface_index = i;
          }
        }
      }
      if(end == v){
        here_v_exists = true;
        current->last_updated = get_struct_timeval();
        here_v = current;
        /*Check if the next hop is u (ip), if yes and the route is garbage
        we need to broadcast that, remove the entry and return*/
        if(here_v->next_hop_ip == ip && received->metric > 15){
          fprintf(stderr, "%s\n", "Using a dirty route! Broadcast and remove...");
          here_v->is_garbage = 1;
          broadcast_single_entry(here_v);
          remove(here_v);
          print_routing_table(head_rt);
          return;
        }
      }
      current = current->next;
    }
    if(!here_u_exists && !v_same_as_here){ //This connection doesn't exist, add
      here_u = (route_t *) malloc(sizeof(route_t));
      here_u->subnet = ip;
      here_u->next_hop_ip = 0; //This is a direct connection
      for(uint32_t i=0;i<dr_interface_count();i++){
        lvns_interface_t tmp = dr_get_interface(i);
        if(((tmp.ip & tmp.subnet_mask) == (ip & tmp.subnet_mask)) && tmp.enabled){ //We received drX --> drHere
          u_interface_index = i;
          //we have found the correct interface
          here_u->outgoing_intf = i;
          here_u->cost = tmp.cost;
          here_u->mask = tmp.subnet_mask;
          here_u->last_updated = get_struct_timeval();
          here_u->learned_from = 0;
          here_u->is_garbage = 0;
          here_u->next = NULL;
          //Append to the list
          if(here_u->cost <= 15){
            append(head_rt, here_u);
            broadcast_single_entry(here_u);
            if (DEBUG) fprintf(stderr, "%s\n", "Added a new entry to the RT.");
            print_routing_table(head_rt);
            here_u_exists = true;
          }
          break;
        }
      }
    }
    if(!here_v_exists && !v_same_as_here && u_interface_index != -1){
      here_v = (route_t *) malloc(sizeof(route_t));
      here_v->subnet = received->ip; //received = u -> v
      here_v->mask = received->subnet_mask;
      here_v->next_hop_ip = ip; //Hop to u first
      here_v->outgoing_intf = u_interface_index; //Intf index to send out packets to u
      here_v->cost = here_u->cost + received->metric;
      here_v->last_updated = get_struct_timeval();
      here_v->learned_from = ip;
      here_v->is_garbage = 0;
      here_v->next = NULL;
      if(here_v->cost <= 15){
        append(head_rt, here_v);
        broadcast_single_entry(here_v);
        here_v_exists = true;
        fprintf(stderr, "%s\n", "Added here -> v");
        print_routing_table(head_rt);
      }
    } else if(!v_same_as_here && u_interface_index != -1 && here_u_exists){ /*Bellman Ford update*/
      if(here_v->cost > here_u->cost + received->metric){
        fprintf(stderr, "%s", "Bellman Ford update of route here -> ");
        print_ip(here_v->subnet);
        fprintf(stderr, "%d > %d + %d\n",here_v->cost, here_u->cost, received->metric );
        here_v->cost = here_u->cost + received->metric;
        here_v->outgoing_intf = u_interface_index;
        here_v->next_hop_ip = here_u->subnet;
        here_v->mask = here_u->mask;
        here_v->learned_from = ip;
        print_routing_table(head_rt);
        /*Triggered update: Send out this packet immediately*/
        broadcast_single_entry(here_v);
      }
    }
    free(header);
    free(received);
}

void safe_dr_handle_periodic() {
    /* handle periodic tasks for dynamic routing here */
    /*Send out the complete routing table to neighbors*/
    advertise_routing_table();

    long current_time;
    route_t *current = head_rt;
    while(current != NULL){
      current_time = get_time();
      long time_entry = current->last_updated.tv_sec * 1000 + current->last_updated.tv_usec / 1000;
      if((current_time - time_entry)/1000.f > RIP_TIMEOUT_SEC){ //Convert difference to seconds
        current->is_garbage = 1;
        fprintf(stderr, "%s", "Garbage IP: ");
        print_ip(current->subnet);
        broadcast_single_entry(current);
        remove(current);
        print_routing_table(head_rt);
      }
      current = current->next;
    }

}

static void safe_dr_interface_changed(unsigned intf,
                                      int state_changed,
                                      int cost_changed) {
    /* handle an interface going down or being brought up */

    /*If state_changed, look at the interface and see if it is enabled
        EN:  add u -> subnet (n_hop_ip = 0) and broadcast to all neighbors
        DIS: for all entries in the RT, which use intf, is_garbage = 1, broadcast, remove
      If cost_changed
        for all entries in the RT that use this intfc, is_garbage = 1, broadcast, new cost + is_garbage = 0, broadcast direct link to subnet */

    lvns_interface_t tmp = dr_get_interface(intf);
    route_t *current = head_rt;
    route_t *new_entry;
    if(state_changed){
      bool EN = (tmp.enabled != 0);
      if(EN){
        new_entry = (route_t *) malloc(sizeof(route_t));
        new_entry->subnet = tmp.ip & tmp.subnet_mask;
        new_entry->mask = tmp.subnet_mask;
        new_entry->next_hop_ip = 0;
        new_entry->outgoing_intf = intf;
        new_entry->cost = tmp.cost;
        new_entry->last_updated = get_struct_timeval();
        new_entry->learned_from = 0;
        new_entry->is_garbage = 0;
        new_entry->next = NULL;
        append(head_rt, new_entry);
        broadcast_single_entry(new_entry);
      } else{
        broadcast_intf_down(tmp.ip);
        while(current->next != NULL){
          if(current->outgoing_intf == intf){
            current->cost = INFINITY;
            broadcast_single_entry(current);
            remove(current);
          }
          current = current->next;
        }
      }
    } else if(cost_changed){
      while (current != NULL) {
        if(current->outgoing_intf == intf){
          current->is_garbage = 1;
          broadcast_single_entry(current);
          remove(current);
        }
        current = current->next;
      }
      new_entry = (route_t *) malloc(sizeof(route_t));
      new_entry->subnet = tmp.ip & tmp.subnet_mask;
      new_entry->mask = tmp.subnet_mask;
      new_entry->next_hop_ip = 0;
      new_entry->outgoing_intf = intf;
      new_entry->cost = tmp.cost;
      new_entry->last_updated = get_struct_timeval();
      new_entry->learned_from = 0;
      new_entry->is_garbage = 0;
      new_entry->next = NULL;
      append(head_rt, new_entry);
      broadcast_single_entry(new_entry);
    } else {
      return;
    }
}

/* definition of internal functions */

void broadcast_intf_down(uint32_t intf_ip){
  rip_entry_t *packet = (rip_entry_t *) malloc(sizeof(rip_entry_t));
  rip_header_t *header = (rip_header_t *) malloc(sizeof(rip_header_t));
  packet->addr_family = IPV4_ADDR_FAM;
  packet->pad = 0;
  packet->ip = intf_ip;
  packet->next_hop = intf_ip;
  header->command = RIP_COMMAND_RESPONSE;
  header->version = RIP_VERSION;
  header->pad = 0;
  char buf[sizeof(*header) + sizeof(*packet)];
  memcpy(buf, header, sizeof(*header));
  memcpy(buf + sizeof(*header), packet, sizeof(*packet));
  for(uint32_t i=0;i<dr_interface_count();i++){
      if(dr_get_interface(i).enabled){
        dr_send_payload(RIP_IP, RIP_IP, i,buf,sizeof(buf));
      }
    }
  free(packet);
  free(header);
}
void broadcast_single_entry(route_t *to_broadcast){
  for(uint32_t i=0;i<dr_interface_count();i++){
      if(dr_get_interface(i).enabled){
      rip_entry_t *packet = (rip_entry_t *) malloc(sizeof(rip_entry_t));
      rip_header_t *header = (rip_header_t *) malloc(sizeof(rip_header_t));
      packet->addr_family = IPV4_ADDR_FAM;
      packet->pad = 0;
      packet->ip = to_broadcast->subnet;
      packet->subnet_mask = to_broadcast->mask;
      packet->next_hop = to_broadcast->next_hop_ip;
      packet->learned_from = to_broadcast->learned_from;
      if(to_broadcast->is_garbage == 1){
        packet->metric = INFINITY;
      } else{
        packet->metric = to_broadcast->cost;
      }
      header->command = RIP_COMMAND_RESPONSE;
      header->version = RIP_VERSION;
      header->pad = 0;
      char buf[sizeof(*header) + sizeof(*packet)];
      memcpy(buf, header, sizeof(*header));
      memcpy(buf + sizeof(*header), packet, sizeof(*packet));
      dr_send_payload(RIP_IP, RIP_IP, i,buf,sizeof(buf));
      free(packet);
      free(header);
    }
  }
}

void advertise_routing_table(){
  route_t *current;
  for(uint32_t i=0;i<dr_interface_count();i++){
    if(dr_get_interface(i).enabled){
      current = head_rt;
      while(current != NULL){
        rip_entry_t *packet = (rip_entry_t *) malloc(sizeof(rip_entry_t));
        rip_header_t *header = (rip_header_t *) malloc(sizeof(rip_header_t));
        packet->addr_family = IPV4_ADDR_FAM;
        packet->pad = 0;
        packet->ip = current->subnet;
        packet->subnet_mask = current->mask;
        packet->next_hop = current->next_hop_ip;
        packet->learned_from = current->learned_from;
        if(current->is_garbage == 1){
          packet->metric = INFINITY;
        } else{
          packet->metric = current->cost;
        }
        header->command = RIP_COMMAND_RESPONSE;
        header->version = RIP_VERSION;
        header->pad = 0;

        char buf[sizeof(*header) + sizeof(*packet)];
        memcpy(buf, header, sizeof(*header));
        memcpy(buf + sizeof(*header), packet, sizeof(*packet));

        dr_send_payload(RIP_IP, RIP_IP, i,buf,sizeof(buf));

        free(packet);
        free(header);

        current = current->next;
      }
    } else{
      broadcast_intf_down(dr_get_interface(i).ip);
    }
  }
}

// gives current time in milliseconds
long get_time(){
    // Now in milliseconds
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

void append(route_t *head, route_t *new_entry){
  route_t *current = head;

  while (current->next != NULL) {
    if(current->subnet == new_entry->subnet){
      current->mask = new_entry->mask;
      current->next_hop_ip = new_entry->next_hop_ip;
      current->outgoing_intf = new_entry->outgoing_intf;
      current->cost = new_entry->cost;
      current->last_updated = new_entry->last_updated;
      current->learned_from = new_entry->learned_from;
      current->is_garbage = new_entry->is_garbage;
      return;
    }
    current = current->next;
  }
  if(current->subnet == new_entry->subnet){
    current->mask = new_entry->mask;
    current->next_hop_ip = new_entry->next_hop_ip;
    current->outgoing_intf = new_entry->outgoing_intf;
    current->cost = new_entry->cost;
    current->last_updated = new_entry->last_updated;
    current->learned_from = new_entry->learned_from;
    current->is_garbage = new_entry->is_garbage;
    return;
  }
  current->next = (route_t *) malloc(sizeof(route_t)); //DEBUG:Add catch of false malloc
  current->next = new_entry;
}

void remove(route_t *to_remove){
  route_t *current = head_rt;
  if(to_remove == head_rt){
    if(head_rt->next != NULL){
      head_rt = head_rt->next;
    } else head_rt = NULL;
  } else{
    while(current->next != to_remove){
      current = current->next;
    }
    if(current->next == to_remove){ //Could be NULL
      current->next = to_remove->next;
      free(to_remove);
    }
  }
}

void print_packet(rip_entry_t *packet){
  fprintf(stderr, " Packet IP: ");
  print_ip(packet->ip);
  fprintf(stderr, " Subnet mask: ");
  print_ip(packet->subnet_mask);
  fprintf(stderr, " Next hop: ");
  print_ip(packet->next_hop);
}

uint32_t count_route_table_entries(){
  uint32_t cnt = 0;
  route_t *current = head_rt;

  while (current->next != NULL) {
    cnt++;
    current = current->next;
  }
  return cnt;
}

struct timeval get_struct_timeval(){
  struct timeval now;
  gettimeofday(&now, NULL);
  return now;
}

// prints an ip address in the correct format
// this function is taken from:
// https://stackoverflow.com/questions/1680365/integer-to-ip-address-c
void print_ip(int ip)
{
    unsigned char bytes[4];
    bytes[0] = ip & 0xFF;
    bytes[1] = (ip >> 8) & 0xFF;
    bytes[2] = (ip >> 16) & 0xFF;
    bytes[3] = (ip >> 24) & 0xFF;
    printf("%d.%d.%d.%d\n", bytes[3], bytes[2], bytes[1], bytes[0]);
}

// prints the full routing table
void print_routing_table(route_t *head){
    printf("==================================================================\nROUTING TABLE:\n==================================================================\n");
    int counter = 0;
    route_t *current = head;
    while (current != NULL){
        printf("Entry %d:\n",counter);
        printf("\tSubnet: ");
        print_ip(current->subnet);
        printf("\tMask: ");
        print_ip(current->mask);
        printf("\tNext hop ip: ");
        print_ip(current->next_hop_ip);
        printf("\tOutgoing interface: ");
        print_ip(current->outgoing_intf);
        printf("\tCost: %d\n", current->cost);
        printf("\tLast updated (timestamp in microseconds): %li \n", current->last_updated.tv_usec);
        printf("==============================\n");
        counter ++;

        current = current->next;
    }
}
