#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <enet/enet.h>
#include <enet/types.h>
#include <time.h>

#define MS_VERSION "0.2"
#define MS_MAXSRVS 128
#define MS_TIMEOUT 100

#define NET_CHANS 2
#define NET_CH_MAIN 0
#define NET_CH_UPD  1
#define NET_MAXCLIENTS 64

#define NET_BUFSIZE 65536

#define NET_MSG_ADD  200
#define NET_MSG_RM   201
#define NET_MSG_LIST 202

#define LC_MS_INIT "D2DF master server starting on port %d...\n"
#define LC_MS_ADD  "\nAdded server in slot #%d:\n%s:%d\n%s\n%s (%d)\n%d/%d plrs\nproto: %d pw?: %d\n"
#define LC_MS_UPD  "\nUpdated server #%d (%s:%d):\n%s\n%s (%d)\n%d/%d plrs\nproto: %d pw?: %d\n"
#define LC_MS_RM   "\nRemoved server #%d (%s:%d) by request.\n"
#define LC_MS_TIME "\nServer #%d (%s:%d) timed out.\n"
#define LC_MS_LIST "\nSent server list to %x:%u (ver. %s).\n"
#define LC_MS_DIE  "\nD2DF master server shutting down...\n"
#define LC_MS_CONN "\nIncoming connection from %x:%u...\n"
#define LC_MS_MOTD "\nMOTD: %s\n"
#define LC_MS_URGENT "\nURGENT: %s\n"

#define MS_URGENT_FILE "urgent.txt"
#define MS_MOTD_FILE "motd.txt"

struct ms_server_s {
  enet_uint8 used;
  char s_ip[17];
  char s_name[256];
  char s_map[256];
  enet_uint8  s_pw;
  enet_uint8  s_plrs;
  enet_uint8  s_maxplrs;
  enet_uint8  s_mode;
  enet_uint8  s_protocol;
  enet_uint16 s_port;
  time_t      deathtime;
};

typedef struct ms_server_s ms_server;

const char ms_game_ver[] = "0.63";
char ms_motd[255] = "";
char ms_urgent[255] = "";

int ms_port = 25660;
int ms_timeout = 100;

size_t b_read = 0;
size_t b_write = 0;

enet_uint8 b_send[NET_BUFSIZE];

ENetHost  *ms_host = NULL;
ENetPeer  *ms_peers[NET_MAXCLIENTS];
ms_server  ms_srv[MS_MAXSRVS];
enet_uint8 ms_count = 0;

// fake servers to show on old versions of the game
ms_server ms_fake_srv[] = {
  {
    .used = 1,
    .s_ip = "0.0.0.0",
    .s_name = "! \xc2\xc0\xd8\xc0 \xca\xce\xcf\xc8\xdf \xc8\xc3\xd0\xdb "
              "\xd3\xd1\xd2\xc0\xd0\xc5\xcb\xc0! "
              "\xd1\xca\xc0\xd7\xc0\xc9\xd2\xc5 \xcd\xce\xc2\xd3\xde C "
              "doom2d.org !",
    .s_map = "! Your game is outdated. "
             "Get latest version at doom2d.org !",
    .s_protocol = 255,
  },
  {
    .used = 1,
    .s_ip = "0.0.0.0",
    .s_name = "! \xcf\xd0\xce\xc1\xd0\xce\xd1\xdcTE \xcf\xce\xd0\xd2\xdb "
              "25666 \xc8 57133 HA CEPBEPE \xcf\xc5\xd0\xc5\xc4 \xc8\xc3\xd0\xce\xc9 !",
    .s_map = "! Forward ports 25666 and 57133 before hosting !",
    .s_protocol = 255,
  },
};

#define MS_FAKESRVS (sizeof(ms_fake_srv) / sizeof(ms_fake_srv[0]))

void i_usage () {
  printf("Usage: d2df_master -p port_number [-t timeout_seconds]\n");
  fflush(stdout);
}


void i_version () {
  printf("Doom 2D Forever master server v%s\n", MS_VERSION);
  fflush(stdout);
}


void d_error (const char *msg, int fatal) {
  if (fatal) {
    fprintf(stderr, "FATAL ERROR: %s\n", msg);
    exit(EXIT_FAILURE);
  } else {
    fprintf(stderr, "ERROR: %s\n", msg);
  }
}


void d_getargs (int argc, char *argv[]) {
  if (argc < 2) {
    i_usage();
    exit(0);
    return;
  }

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-v")) {
      i_version();
      exit(0);
      return;
    } else if (!strcmp(argv[i], "-p")) {
      if (i + 1 >= argc) {
        d_error("Specify a port value!", 1);
        return;
      } else {
        ms_port = atoi(argv[++i]);
      }
    } else if (!strcmp(argv[i], "-t") & (i + 1 < argc)) {
        ms_timeout = atoi(argv[++i]);
    }
  }
}





enet_uint8 b_read_uint8 (enet_uint8 buf[], size_t *pos) {
  return buf[(*pos)++];
}


enet_uint16 b_read_uint16 (enet_uint8 buf[], size_t *pos) {
  enet_uint16 ret = 0;

  ret = *(enet_uint16*)(buf + *pos);
  *pos += sizeof(enet_uint16);

  return ret;
}


char* b_read_dstring (enet_uint8 buf[], size_t *pos) {
  char *ret = NULL;

  size_t len = b_read_uint8(buf, pos);

  ret = malloc(len + 1);

  memmove(ret, (char*)(buf + *pos), len);
  ret[len] = '\0';
  *pos += len;

  return ret;
}


void b_write_uint8 (enet_uint8 buf[], size_t *pos, enet_uint8 val) {
  buf[(*pos)++] = val;
}


void b_write_uint16 (enet_uint8 buf[], size_t *pos, enet_uint16 val) {
  *(enet_uint16*)(buf + *pos) = val;
  *pos += sizeof(enet_uint16);
}


void b_write_dstring (enet_uint8 buf[], size_t *pos, const char* val) {
  enet_uint8 len = strlen(val);
  b_write_uint8(buf, pos, len);

  memmove((char*)(buf + *pos), val, len);
  *pos += len;
}


void b_write_server (enet_uint8 buf[], size_t *pos, ms_server s) {
//  b_write_dstring(b_send, pos, s.s_ip);
 //msg = b_read_uint8(event.packet->data, &b_read);
  b_write_uint8(b_send, pos,  NET_MSG_ADD);
  b_write_uint16 (b_send, pos, s.s_port);
  b_write_dstring(b_send, pos, s.s_name);
  b_write_dstring(b_send, pos, s.s_map);
  b_write_uint8  (b_send, pos, s.s_mode);
  b_write_uint8  (b_send, pos, s.s_plrs);
  b_write_uint8  (b_send, pos, s.s_maxplrs);
  b_write_uint8  (b_send, pos, s.s_protocol);
  b_write_uint8  (b_send, pos, s.s_pw);
}



void b_write_player (enet_uint8 buf[], size_t *pos) {
//  b_write_dstring(b_send, pos, s.s_ip);
 //msg = b_read_uint8(event.packet->data, &b_read);
  b_write_uint8(b_send, pos,  NET_MSG_ADD);
  b_write_uint8 (b_send, pos, rand());
  b_write_dstring(b_send, pos, "");
  b_write_dstring(b_send, pos, "spamir");
  b_write_uint8  (b_send, pos, "GAVNO");
  b_write_uint8  (b_send, pos, rand());
  b_write_uint8  (b_send, pos, rand());
  b_write_uint8  (b_send, pos, rand());
  b_write_uint8  (b_send, pos, rand());
}


int main (int argc, char *argv[]) {
  d_getargs(argc, argv);

  if (enet_initialize()) {
    d_error("Could not init ENet!", 1);
    return EXIT_FAILURE;
  }
  ENetHost * client;
  client = enet_host_create (NULL /* create a client host */,
            NET_MAXCLIENTS /* only allow 1 outgoing connection */,
            NET_CHANS /* allow up 2 channels to be used, 0 and 1 */,
            0 /* assume any amount of incoming bandwidth */,
            0 /* assume any amount of outgoing bandwidth */);
if (client == NULL)
{
    fprintf (stderr, 
             "An error occurred while trying to create an ENet client host.\n");
    exit (EXIT_FAILURE);
}
printf("Created ENet client.\n");
ENetAddress address;
ENetEvent event;
ENetPeer* peer;
char server[] = "94.19.105.114"; // mpms.doom2d.org:25665, deadsoftware.ru:25665
enet_address_set_host (& address, server);
//address.host = ENET_HOST_ANY;
address.port = 25667;
peer = enet_host_connect (client, & address, 2, 0);    
if (peer == NULL)
{
   fprintf (stderr, 
            "No available peers for initiating an ENet connection.\n");
   exit (EXIT_FAILURE);
}
printf("Connected to host.\n");
if (enet_host_service (client, & event, 100) > 0 &&
    event.type == ENET_EVENT_TYPE_CONNECT)
{
  //printf("EVENT TYPE: %i\n", event.type);
  //printf("Connection to %s is a success!\n", server);
 printf(LC_MS_CONN, event.peer->address.host, event.peer->address.port);
}
else
{
    /* Either the 5 seconds are up or a disconnect event was */
    /* received. Reset the peer in the event the 5 seconds   */
    /* had run out without any significant event.            */
    enet_peer_reset (peer);
    printf("connection to %s is a failure\n", server);
};
printf("post\n");

srand(time(NULL));
char  buffer[1000];
ms_server fakesrv = ms_fake_srv[0];
fakesrv.s_pw='\0';
fakesrv.s_plrs=rand();
fakesrv.s_maxplrs=rand();
fakesrv.s_mode=10;
fakesrv.s_protocol=666;
fakesrv.s_port=25567;
int connected = 1;
int w = 0;
             b_write = 0;
             b_read = 0;
             b_write_player(b_send, &b_write);
             ENetPacket *p = enet_packet_create(b_send, b_write, ENET_PACKET_FLAG_RELIABLE); 
             enet_peer_send(peer, NET_CH_UPD, p);
             enet_host_service(client, &event, 100);
             printf("Sent fake player\n");
while (1) {
    printf("loop\n");
    while (enet_host_service(client, &event, 100)) {

        printf("loop2, %i", event.type);
        switch (event.type) {
 case ENET_EVENT_TYPE_RECEIVE:
 printf("%s got info!", event.packet->data);
break;
 case ENET_EVENT_TYPE_DISCONNECT:
connected=0;
printf("You have been disconnected.\n");
 return 2;
 }
    }
    
             ///// FAKE PLAYER
             //// FAKE PLAYER
    /*
    fakesrv.s_plrs= rand();
    fakesrv.s_maxplrs = rand();
    fakesrv.s_port = rand();
                    strncpy(fakesrv.s_ip, "1.1.1.1", sizeof(fakesrv.s_ip));
                    //strncpy(fakesrv.s_map, "Dungeon, Alabama", sizeof(fakesrv.s_map));
                    if (w == 0 ) {
                      //char *a = printf("%s%i","[world] Dungeon of Masterwock, FREE fisting and hot gay sex! JOIN AND CUM TODAY!", rand() );
                      strncpy(fakesrv.s_map, ms_fake_srv[1].s_map, sizeof(fakesrv.s_map));
                      strncpy(fakesrv.s_name, ms_fake_srv[1].s_name, sizeof(fakesrv.s_name));  
                      w = 1;
                    }
                    else if (w == 1) {
                      //char *a = printf("%s%i","[spb] Lair of Jabberwock, 24/7", rand() );
                      strncpy(fakesrv.s_map, ms_fake_srv[0].s_map, sizeof(fakesrv.s_map));
                      strncpy(fakesrv.s_name, ms_fake_srv[0].s_name, sizeof(fakesrv.s_name));  
                      w = 0;
                    }
     b_read = 0;
  b_write=0;
   enet_uint8 msg = 255;
  char *ip = NULL;
  enet_uint16 port = 0;
  char *name = NULL;
  char *map = NULL;
  char *clientver = NULL;
  enet_uint8 gm = 0;
  enet_uint16 pl = 0;
  enet_uint16 mpl = 0;
  enet_uint8 proto = 0;
  enet_uint8 pw = 0;
  b_write_server(b_send, &b_write, fakesrv);
     ENetPacket *p = enet_packet_create(b_send, b_write, ENET_PACKET_FLAG_RELIABLE); 
     enet_peer_send(peer, NET_CH_UPD, p);
     b_read=0;
     printf("Sent packet to %s\n", server );
              b_read = 0;
              b_write = 0;
              enet_host_service(client, &event, 100);
              */
}
}