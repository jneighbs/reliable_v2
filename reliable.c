
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define BuffLength 500
#define DataPacketHeaderSize 12
#define AckPackSize 8

typedef struct packetCapsule packetCapsule;

struct packetCapsule {
  packetCapsule* next;
  packetCapsule* prev;
  packet_t* currData;
};

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */
  uint32_t sequenceNumber;
  uint32_t ackNumber;
  packetCapsule* head;
  packetCapsule* tail;

  int waitingForAck;
  int moreDataToSend;

  int receivedBlankDataPacket;
  int allSentPacketsAcked;
  int haveWrittenAllOutput;

  int numDataPacksToBeSent;
  int numAcksReceived;
  int numBytesReceived;
  int numBytesOutputted;

  struct timespec time;

};
rel_t *rel_list;









/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;

  r->sequenceNumber = 1;
  r->ackNumber = 1;

  r->head = NULL;
  r->tail=NULL;

  r->waitingForAck = 0;
  r->moreDataToSend = 0;

  r->receivedBlankDataPacket=0;
  r->allSentPacketsAcked=1;
  r->haveWrittenAllOutput=1;
  

  r->numDataPacksToBeSent = 0;
  r->numAcksReceived = 0;
  r->numBytesReceived = 0;
  r->numBytesOutputted = 0;

  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  return r;
}

/*
Checks the conditions to see if it is time to tear down the program
 */
void
checkIfShouldTeardown(rel_t *r){
  if(r->receivedBlankDataPacket&&r->allSentPacketsAcked&&r->haveWrittenAllOutput)
    rel_destroy(r);
}

void
rel_destroy (rel_t *r)
{
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  if(r->tail!=NULL){
    while(r->tail->prev!=NULL){
      free(r->tail->currData);
      r->tail  = r->tail->prev;
      free(r->tail->next);
    }
    free(r->tail->currData);
    free(r->tail);
  }
  free(r);
}

/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{
}


void sendDataPacket(rel_t *s){
  //check for timeout
  clock_gettime(CLOCK_MONOTONIC, &(s->time));
  if(s->waitingForAck) return;
  if(s->head == NULL) return;
  /*
if(EOFPacketHasBeenAcked()){
s->allSentPacketsAcked = 1;
checkIfDestroy(s);
}
   */

  conn_sendpkt(s->c, s->head->currData,  ntohs(s->head->currData->len));

 s->waitingForAck = 1;
 s->allSentPacketsAcked = 0;

}


void 
sendAckPack(rel_t* s){

  struct ack_packet* ackPack = malloc(AckPackSize);
  ackPack->cksum = 0;
  ackPack->len = htons(sizeof(*ackPack));
  ackPack->ackno = htonl(s->ackNumber);
  ackPack->cksum = cksum(ackPack, sizeof(*ackPack));

  conn_sendpkt(s->c, ((packet_t*)ackPack), sizeof(ackPack));

 

}

int
correctAckNoReceived(rel_t* r, packet_t* pkt){
  if(r->sequenceNumber == (pkt->ackno)-1)return 1;
  return 0;
}

void
swapPacketByteOrderToHost(packet_t* pkt){
  pkt->len = ntohs(pkt->len);
  pkt->ackno = ntohl(pkt->ackno);
  if(pkt->len > AckPackSize)
    pkt->seqno = ntohl(pkt->seqno);
}

void
swapPacketByteOrderToNetworkk(packet_t* pkt){
  pkt->len = htons(pkt->len);
  pkt->ackno = htonl(pkt->ackno);
  if(pkt->len > AckPackSize)
    pkt->seqno = htonl(pkt->seqno);
}



//sanity check - checks the checksum, etc to make sure the packet is
//correctly formed. Also converts packet to host byte order. The int being
//returned is meant to represent the type of packet returned, and should
//be used in a switch statement.
int
sanityCheck(rel_t* r,packet_t* pkt,size_t n){


  //chksum - veryify that it matches
  uint16_t packetChecksum = pkt->cksum;
  pkt->cksum = 0;
  uint16_t actualChecksum = cksum(pkt, n);
  if(packetChecksum != actualChecksum) return -1;

  //verify packet length
  swapPacketByteOrderToHost(pkt);
  uint16_t packetLen = pkt->len;
  if (packetLen!=n)return -1;

  //make sure there is room in buffer
  size_t bufferSpace = conn_bufspace(r->c);
  if(bufferSpace < packetLen && packetLen > DataPacketHeaderSize && packetLen <=DataPacketHeaderSize + BuffLength)return 4;

  if(packetLen == AckPackSize)return 1;
  if(packetLen > DataPacketHeaderSize && packetLen <=DataPacketHeaderSize + BuffLength)return 2;
  if(packetLen == DataPacketHeaderSize||r->receivedBlankDataPacket )return 3;
  
  
  return -1;//malformed
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{

  int packetStatus = sanityCheck(r,pkt,n);

  switch(packetStatus){

  case(1)://ackPack

    if(correctAckNoReceived(r, pkt)){
      r->sequenceNumber++;
      r->numAcksReceived++;
      r->head = r->head->next;
      r->waitingForAck = 0;
      sendDataPacket(r);
 	
      if(r->numAcksReceived == r->numDataPacksToBeSent){
	r->allSentPacketsAcked = 1;
	checkIfShouldTeardown(r);
      } else{
	r->allSentPacketsAcked = 0;
      }
	
     
    }

    break;

  case(2)://dataPack

    if(pkt->seqno == r-> ackNumber){
      r->ackNumber++;
      sendAckPack(r);
      r->numBytesReceived += ((pkt->len)-DataPacketHeaderSize);
      int bytesOutputted = conn_output(r->c, pkt->data,   n-DataPacketHeaderSize);
      r->numBytesOutputted += bytesOutputted;
      if(r->numBytesReceived == r->numBytesOutputted)
	r->haveWrittenAllOutput = 1;

    } else {
      sendAckPack(r);
    }

    break;

  case(3)://EOF/empty pack

    r->ackNumber++;
    sendAckPack(r);
    r->receivedBlankDataPacket = 1;
    if(r->numBytesReceived == r->numBytesOutputted){
      r->haveWrittenAllOutput=1;
      conn_output(r->c, pkt->data, 0);
    }	 
    checkIfShouldTeardown(r);

    break;

  case(4)://dataPack - no room in buffer

    //int buffSpace = conn_bufspace(r->c);
    //trimAndSend(buffSpace, pkt);
    break;

  case(-1)://malformed
    break;
  }
}


packet_t* stuffIntoPacket(rel_t* s, char* data, int len){//len can be 0 or 1-500

 			 
  (s->numDataPacksToBeSent)++;
  packet_t* pkt = malloc((len+DataPacketHeaderSize) * sizeof(char)); //dont forget to free  me!
  pkt->cksum = 0;
  pkt->len = htons(len+DataPacketHeaderSize);
  pkt->ackno = htonl(1);//no piggybacking
  pkt->seqno = htonl(s->numDataPacksToBeSent);

  memcpy(pkt->data, data, len);
 
  pkt->cksum =  cksum(pkt, len+DataPacketHeaderSize);

  return pkt;
 

}

packet_t* grabData(rel_t* s){
  //at this point, s-> moreDataToSend must ==1

 
  char buf[BuffLength];
  int packetLength;
  int numBytesOfData = conn_input(s->c,buf,BuffLength-1); //either -1, 0,  1-499


  if(numBytesOfData == -1){//EOF, send empty packet
    packetLength=0;
    s->receivedBlankDataPacket = 1;

  } else if(numBytesOfData == 0){//no more data to send in input buffer
    s->moreDataToSend = 0;
    return NULL;

  } else{ //for data to be sent
    buf[numBytesOfData] = '\0';
    packetLength = numBytesOfData + 1;

  }
 
  packet_t* pkt = stuffIntoPacket(s, buf, packetLength);

  return pkt;
 
}

  
void appendToPacketList(rel_t *s, packet_t* pkt){
 
  if(s->moreDataToSend == 0)return;
  if(ntohs(pkt->len) == DataPacketHeaderSize)s->moreDataToSend = 0;//EOF
 
  packetCapsule* curr = malloc(sizeof(*curr));//dont forget to free me!!
  curr->next = NULL;
  curr->prev = NULL;
  curr->currData =  pkt;

  if(s->head==NULL){
    s->head = curr;
    curr->prev = s->tail;
    s->tail = curr;
  } else {
    curr->prev = s->tail;
    s->tail->next = curr;
    s->tail = curr;
  }
  
}

 

void
rel_read (rel_t *s)
{
  s->moreDataToSend = 1;
   while(s->moreDataToSend){
    packet_t* pkt = grabData(s);
    appendToPacketList(s,pkt);
     }
  sendDataPacket(s);
}

void
rel_output (rel_t *r)
{
  /*  
  int buffSpace = conn_bufspace(r->c);
  if( r->outputBufferData != NULL)
    if( buffSpace >= r->outputBufferData){
      conn_output();
      send ack;
      else{
	trimAndSend(buffSpace, r->outputBufferData);
      }
    }
  */
  if(r->receivedBlankDataPacket && r->haveWrittenAllOutput){

    //send eof through
  }  
}



void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */
  struct timespec t2;
  clock_gettime(CLOCK_MONOTONIC, &t2);



}
