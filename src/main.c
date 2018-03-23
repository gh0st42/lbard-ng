/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015 Serval Project Inc.

This program monitors a local Rhizome database and attempts
to synchronise it over low-bandwidth declarative transports, 
such as bluetooth name or wifi-direct service information
messages.  It is intended to give a high priority to MeshMS
converations among nearby nodes.

The design is fully asynchronous, so a call to the update_my_message()
function from time to time should be all that is required.


This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#include "sync.h"
#include "lbard.h"
#include "serial.h"
#include "util.h"

int debug_radio=0;
int debug_pieces=0;
int debug_announce=0;
int debug_pull=0;
int debug_insert=0;
int debug_radio_rx=0;
int debug_gpio=0;
int debug_message_pieces=0;
int debug_sync=0;
int debug_sync_keys=0;
int debug_bundlelog=0;
int debug_noprioritisation=0;

int radio_silence_count=0;

int http_server=1;
int udp_time=0;
int time_slave=0;
int time_server=0;
char *time_broadcast_addrs[]={DEFAULT_BROADCAST_ADDRESSES,NULL};


int reboot_when_stuck=0;
extern int serial_errors;

unsigned char my_sid[32];
char *my_sid_hex=NULL;
unsigned int my_instance_id;


char *servald_server="";
char *credential="";
char *prefix="";

char *token=NULL;

time_t last_summary_time=0;
time_t last_status_time=0;

int monitor_mode=0;

struct sync_state *sync_state=NULL;

int urandombytes(unsigned char *buf, size_t len)
{
  static int urandomfd = -1;
  int tries = 0;
  if (urandomfd == -1) {
    for (tries = 0; tries < 4; ++tries) {
      urandomfd = open("/dev/urandom",O_RDONLY);
      if (urandomfd != -1) break;
      sleep(1);
    }
    if (urandomfd == -1) {
      perror("open(/dev/urandom)");
      return -1;
    }
  }
  tries = 0;
  while (len > 0) {
    ssize_t i = read(urandomfd, buf, (len < 1048576) ? len : 1048576);
    if (i == -1) {
      if (++tries > 4) {
        perror("read(/dev/urandom)");
        if (errno==EBADF) urandomfd=-1;
        return -1;
      }
    } else {
      tries = 0;
      buf += i;
      len -= i;
    }
  }
  return 0;
}

long long start_time=0;

int main(int argc, char **argv)
{
  fprintf(stderr,"Version 20160927.1311.1\n");
  
  start_time = gettime_ms();
  
  sync_setup();

  // Generate a unique transient instance ID for ourselves.
  // Must be non-zero, as we use zero as a marker for not having yet heard the
  // instance ID of a peer.
  my_instance_id=0;
  while(my_instance_id==0)
    urandombytes((unsigned char *)&my_instance_id,sizeof(unsigned int));

  // MeshMS operations via HTTP, so that we can avoid direct database modification
  // by scripts on the mesh extender devices, and thus avoid database lock problems.
  if ((argc>1)&&!strcasecmp(argv[1],"meshms")) {
    return(meshms_parse_command(argc,argv));
  }

  char *serial_port = "/dev/null";

  if ((argc==3)
      &&((!strcasecmp(argv[1],"monitor"))
	 ||
	 (!strcasecmp(argv[1],"monitorts"))
	 )
      )
    {
      if (!strcasecmp(argv[1],"monitorts")) { time_server=1; udp_time=1; }
      monitor_mode=1;
      serial_port=argv[2];
    } else {  
    if (argc<5) {
      fprintf(stderr,"usage: lbard <servald hostname:port> <servald credential> <my sid> <serial port> [options ...]\n");
      fprintf(stderr,"usage: lbard monitor <serial port>\n");
      fprintf(stderr,"usage: lbard meshms <meshms command>\n");
      fprintf(stderr,"usage: energysamplecalibrate <args>\n");
      fprintf(stderr,"usage: energysamplemaster <args>\n");
      fprintf(stderr,"usage: energysample <args>\n");
      exit(-1);
    }
    serial_port = argv[4];
  }

  int serialfd=-1;
  serialfd = open(serial_port,O_RDWR);
  if (serialfd<0) {
    perror("Opening serial port in main");
    exit(-1);
  }
  if (serial_setup_port(serialfd))
    {
      fprintf(stderr,"Failed to setup serial port. Exiting.\n");
      exit(-1);
    }
  fprintf(stderr,"Serial port open as fd %d\n",serialfd);

      
  int n=5;
  while (n<argc) {
    if (argv[n]) {
      if (!strcasecmp("monitor",argv[n])) monitor_mode=1;
      else if (!strcasecmp("meshmsonly",argv[n])) { meshms_only=1;
	fprintf(stderr,"Only MeshMS bundles will be carried.\n");
      }
      else if (!strncasecmp("minversion=",argv[n],11)) {
	int day,month,year;
	min_version=strtoll(&argv[n][11],NULL,10)*1000LL;
	if (sscanf(argv[n],"minversion=%d/%d/%d",&year,&month,&day)==3) {
	  // Minimum date has been specified using year/month/day
	  // Calculate min_version from that.
	  struct tm tm;
	  bzero(&tm,sizeof(struct tm));
	  tm.tm_mday=day;
	  tm.tm_mon=month-1;
	  tm.tm_year=year-1900;
	  time_t thetime = mktime(&tm);
	  min_version=((long long)thetime)*1000LL;
	}
	time_t mv=(min_version/1000LL);

	// Get minimum time as non NL terminated string
	char stringtime[1024];
	snprintf(stringtime,1024,"%s",ctime(&mv));
	if (stringtime[strlen(stringtime)-1]=='\n') stringtime[strlen(stringtime)-1]=0;

	fprintf(stderr,"Only bundles newer than epoch+%lld msec (%s) will be carried.\n",
		(long long)min_version,stringtime);
      }
      else if (!strcasecmp("rebootwhenstuck",argv[n])) reboot_when_stuck=1;
      else if (!strcasecmp("timeslave",argv[n])) time_slave=1;
      else if (!strcasecmp("timemaster",argv[n])) time_server=1;
      else if (!strncasecmp("hfplan=",argv[n],7))
	hf_read_configuration(&argv[n][7]);
      else if (!strncasecmp("timebroadcast=",argv[n],14))
	time_broadcast_addrs[0]=strdup(&argv[n][14]);
      else if (!strcasecmp("logrejects",argv[n])) debug_insert=1;
      else if (!strcasecmp("pull",argv[n])) debug_pull=1;
      else if (!strcasecmp("radio",argv[n])) debug_radio=1;
      else if (!strcasecmp("pieces",argv[n])) debug_pieces=1;
      else if (!strcasecmp("announce",argv[n])) debug_announce=1;
      else if (!strcasecmp("insert",argv[n])) debug_insert=1;
      else if (!strcasecmp("radio_rx",argv[n])) debug_radio_rx=1;      
      else if (!strcasecmp("gpio",argv[n])) debug_gpio=1;
      else if (!strcasecmp("message_pieces",argv[n])) debug_message_pieces=1;
      else if (!strcasecmp("sync",argv[n])) debug_sync=1;
      else if (!strcasecmp("sync_keys",argv[n])) debug_sync_keys=1;
      else if (!strcasecmp("udptime",argv[n])) udp_time=1;
      else if (!strcasecmp("bundlelog",argv[n])) debug_bundlelog=1;
      else if (!strcasecmp("nopriority",argv[n])) debug_noprioritisation=1;
      else if (!strcasecmp("nohttpd",argv[n])) http_server=0;
      else {
	fprintf(stderr,"Illegal mode '%s'\n",argv[n]);
	exit(-3);
      }
    }
    n++;
  }

  if (message_update_interval<0) message_update_interval=0;
  
  last_message_update_time=0;
  congestion_update_time=0;
  
  my_sid_hex="00000000000000000000000000000000";
  prefix="000000";
  if (!monitor_mode) {
    prefix=strdup(argv[3]);
    if (strlen(prefix)<32) {
      fprintf(stderr,"You must provide a valid SID for the ID of the local node.\n");
      exit(-1);
    }
    prefix[6]=0;  
    if (argc>3) {
      // set my_sid from argv[3]
      for(int i=0;i<32;i++) {
	char hex[3];
	hex[0]=argv[3][i*2];
	hex[1]=argv[3][i*2+1];
	hex[2]=0;
	my_sid[i]=strtoll(hex,NULL,16);
      }
      my_sid_hex=argv[3];
    }
  }

  printf("My SID prefix is %02X%02X%02X%02X%02X%02X\n",
	 my_sid[0],my_sid[1],my_sid[2],my_sid[3],my_sid[4],my_sid[5]);
  
  if (argc>2) credential=argv[2];
  if (argc>1) servald_server=argv[1];

  // Open UDP socket to listen for time updates from other LBARD instances
  // (poor man's NTP for LBARD nodes that lack internal clocks)
  int timesocket=-1;
  if (udp_time) {
    timesocket=socket(AF_INET, SOCK_DGRAM, 0);
    if (timesocket!=-1) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(0x5401);
      bind(timesocket, (struct sockaddr *) &addr, sizeof(addr));
      set_nonblock(timesocket);

      // Enable broadcast
      int one=1;
      int r=setsockopt(timesocket, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
      if (r) {
	fprintf(stderr,"WARNING: setsockopt(): Could not enable SO_BROADCAST\n");
      }

    }
  }

  // HTTP Server socket for accepting MeshMS message submission via web form
  // (Used for sending anonymous messages to a help desk for a mesh network).
  int httpsocket=-1;
  if (http_server) {
    httpsocket=socket(AF_INET, SOCK_STREAM, 0);
    if (httpsocket!=-1) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(0x5402);
      int optval = 1;
      setsockopt(httpsocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
      bind(httpsocket, (struct sockaddr *) &addr, sizeof(addr));
      set_nonblock(httpsocket);
      listen(httpsocket,10);
    }
    
  }

  char token[1024]="";
  
  while(1) {

    unsigned char msg_out[LINK_MTU];

    radio_read_bytes(serialfd,monitor_mode);

    load_rhizome_db_async(servald_server,
			  credential, token);

    switch (radio_get_type()) {
    case RADIO_RFD900: uhf_serviceloop(serialfd); break;
    case RADIO_BARRETT_HF: hf_serviceloop(serialfd); break;
    case RADIO_CODAN_HF: hf_serviceloop(serialfd); break;
    case RADIO_RF95: rf_serviceloop(serialfd); break;
    default:
      fprintf(stderr,"ERROR: Connected to unknown radio type.\n");
      exit(-1);
    }
    
    // Deal gracefully with clocks that run backwards from time to time.
    if (last_message_update_time>gettime_ms())
      last_message_update_time=gettime_ms();
    
    if ((gettime_ms()-last_message_update_time)>=message_update_interval) {

      if (!time_server) {
	// Decay my time stratum slightly
	if (my_time_stratum<0xffff)
	  my_time_stratum++;
      } else my_time_stratum=0x0100;
      // Send time packet
      if (udp_time&&(timesocket!=-1)) {
	{
	  // Occassionally announce our time
	  // T + (our stratum) + (64 bit seconds since 1970) +
	  // + (24 bit microseconds)
	  // = 1+1+8+3 = 13 bytes
	  struct timeval tv;
	  gettimeofday(&tv,NULL);    
	  
	  unsigned char msg_out[1024];
	  int offset=0;
	  msg_out[offset++]='T';
	  msg_out[offset++]=my_time_stratum>>8;
	  for(int i=0;i<8;i++)
	    msg_out[offset++]=(tv.tv_sec>>(i*8))&0xff;
	  for(int i=0;i<3;i++)
	    msg_out[offset++]=(tv.tv_usec>>(i*8))&0xff;
	  // Now broadcast on every interface to port 0x5401
	  // Oh that's right, UDP sockets don't have an easy way to do that.
	  // We could interrogate the OS to ask about all interfaces, but we
	  // can instead get away with having a single simple broadcast address
	  // supplied as part of the timeserver command line argument.
	  struct sockaddr_in addr;
	  bzero(&addr, sizeof(addr)); 
	  addr.sin_family = AF_INET; 
	  addr.sin_port = htons(0x5401);
	  int i;
	  for(i=0;time_broadcast_addrs[i];i++) {
	    addr.sin_addr.s_addr = inet_addr(time_broadcast_addrs[i]);
	    errno=0;
	    sendto(timesocket,msg_out,offset,
		   MSG_DONTROUTE|MSG_DONTWAIT
#ifdef MSG_NOSIGNAL
		   |MSG_NOSIGNAL
#endif	       
		   ,(const struct sockaddr *)&addr,sizeof(addr));
	  }
	  // printf("--- Sent %d time announcement packets.\n",i);
	}
	  
	// Check for time packet
	if (timesocket!=-1)
	  {
	    unsigned char msg[1024];
	    int offset=0;
	    int r=recvfrom(timesocket,msg,1024,0,NULL,0);
	    if (r==(1+1+8+3)) {
	      // see rxmessages.c for more explanation
	      offset++;
	      int stratum=msg[offset++];
	      struct timeval tv;
	      bzero(&tv,sizeof (struct timeval));
	      for(int i=0;i<8;i++) tv.tv_sec|=msg[offset++]<<(i*8);
	      for(int i=0;i<3;i++) tv.tv_usec|=msg[offset++]<<(i*8);
	      // ethernet delay is typically 0.1 - 5ms, so assume 5ms
	      tv.tv_usec+=5000;
	      saw_timestamp("          UDP",stratum,&tv);
	    }
	  }	
	}
	if (httpsocket!=-1)
	  {
	    struct sockaddr cliaddr;
	    socklen_t addrlen;
	    int s=accept(httpsocket,&cliaddr,&addrlen);
	    if (s!=-1) {
	      // HTTP request socket
	      printf("HTTP Socket connection\n");
	      // Process socket
	      // XXX This is synchronous to keep things simple,
	      // which is part of why we only check every second or so
	      // for one new connection.  We also don't allow the request
	      // to linger: if it doesn't contain the request almost immediately,
	      // we reject it with a timeout error.
	      http_process(&cliaddr,servald_server,credential,my_sid_hex,s);
	    }
	  }

	if ((!monitor_mode)&&(radio_ready())) {
	  update_my_message(serialfd,
			    my_sid,
			    LINK_MTU,msg_out,
			    servald_server,credential);
	
	  // Vary next update time by upto 250ms, to prevent radios getting lock-stepped.
	  last_message_update_time=gettime_ms()+(random()%message_update_interval_randomness);
	}
	
	// Update the state file to help debug things
	// (but not too often, since it is SLOW on the MR3020s
	//  XXX fix all those linear searches, and it will be fine!)
	if (time(0)>last_status_time) {
	  last_status_time=time(0)+3;
	  status_dump();
	}
    }
    if ((serial_errors>20)&&reboot_when_stuck) {
      // If we are unable to write to the serial port repeatedly for a while,
      // we could be facing funny serial port behaviour bugs that we see on the MR3020.
      // In which case, if authorised, ask the MR3020 to reboot
      system("reboot");
    }
    
    usleep(10000);

    if (time(0)>last_summary_time) {
      last_summary_time=time(0);
      show_progress();
    }    
  }
}
