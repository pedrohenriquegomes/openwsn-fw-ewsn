#include "opendefs.h"
#include "sensors.h"
#include "light.h"
#include "neighbors.h"
#include "openqueue.h"
#include "openserial.h"
#include "opentimers.h"
#include "packetfunctions.h"
#include "scheduler.h"
#include "idmanager.h"
#include "IEEE802154E.h"
#include "sixtop.h"
#include "debugpins.h"
#include "openrandom.h"

//=========================== variables =======================================

light_vars_t        light_vars;

//=========================== prototypes =======================================

// transmitting
void     light_timer_send_cb(opentimer_id_t id);
void     light_send_task_cb(void);
void     light_format_packet(OpenQueueEntry_t* pkt);
// receiving
void     light_timer_fwd_cb(opentimer_id_t id);
void     light_process_packet_at_sink(void);

//=========================== public ===========================================

//=== initialization

/**
\brief Initialize this module.
*/
void light_init(void) {
#ifdef LIGHT_PRINTOUT_READING
   callbackRead_cbt     light_read_cb;
   uint16_t             lux; 
#endif
   
   // clear local variables
   memset(&light_vars,0,sizeof(light_vars_t));
   
#ifdef LIGHT_PRINTOUT_READING
   // printout the current light reading, used to calibrate LUX_THRESHOLD
   if ( light_checkMyId(SENSOR_ID) && sensors_is_present(SENSOR_LIGHT) ) {
      
      light_read_cb     = sensors_getCallbackRead(SENSOR_LIGHT);
      lux               = light_read_cb();
      
      openserial_printInfo(
         COMPONENT_LIGHT,
         ERR_LIGHT_THRESHOLD,
         (errorparameter_t)lux,
         0
      );
   }
#endif
   
   debugpins_rxlight_clr();
   debugpins_txlight_clr();
}

//=== transmitting

/**
\brief Trigger the light app, which can decide to send a packet.
*/
void light_trigger(void) {
   callbackRead_cbt     light_read_cb;
   bool                 iShouldSend;
#ifdef LIGHT_FAKESEND
   uint16_t             numAsnSinceLastEvent;
#endif
   
   // stop if I'm not the SENSOR mote with a light sensor attached
   if ( light_checkMyId(SENSOR_ID)==FALSE || sensors_is_present(SENSOR_LIGHT)==FALSE ) {
      return;
   }
   
   //=== if I get here, I'm the SENSOR mote
   
#ifdef LIGHT_FAKESEND
   // how many cells since the last time I transmitted?
   numAsnSinceLastEvent = ieee154e_asnDiff(&light_vars.lastEventAsn);
   
   // set light_reading to fake high/low value to trigger packets
   if (numAsnSinceLastEvent>LIGHT_FAKESEND_PERIOD) {
      if (light_vars.light_reading<LUX_THRESHOLD) {
         light_vars.light_reading=2*LUX_THRESHOLD;
      } else {
         light_vars.light_reading=0;
      }
   }
#else
   // current light reading
   light_read_cb             = sensors_getCallbackRead(SENSOR_LIGHT);
   light_vars.light_reading  = light_read_cb();
#endif
   
   // detect light state switches
   if (       light_vars.light_state==FALSE && (light_vars.light_reading >= (LUX_THRESHOLD + LUX_HYSTERESIS))) {
      // light was just turned on
      
      light_vars.light_state = TRUE;
      debugpins_txlight_set();
      iShouldSend = TRUE;
   } else if (light_vars.light_state==TRUE  && (light_vars.light_reading <  (LUX_THRESHOLD - LUX_HYSTERESIS))) {
      // light was just turned off
      
      light_vars.light_state = FALSE;
      debugpins_txlight_clr();
      iShouldSend = TRUE;
   } else {
      // light stays in same state
      
      iShouldSend = FALSE;
   }
   
   // abort if no packet to send
   if (iShouldSend==FALSE) {
      return;
   }
   
   //=== if I get here, I will send a packet
   
   // remember the current ASN
   ieee154e_getAsnStruct(&light_vars.lastEventAsn);
   
   // increment the seqnum
   light_vars.seqnum++;
   
   // initiate the burst of packets
   light_vars.numBurstPktsSent = 0;
   
   // start timer for sending packets
   light_vars.sendTimerId = opentimers_start(
      LIGHT_SEND_PERIOD_MS,
      TIMER_PERIODIC,
      TIME_MS,
      light_timer_send_cb
   );
}

void light_timer_send_cb(opentimer_id_t timerId) {
   if (light_vars.numBurstPktsSent<LIGHT_BURSTSIZE) {
      light_vars.numBurstPktsSent++;
      scheduler_push_task(light_send_task_cb, TASKPRIO_MAX);
   } else {
      opentimers_stop(timerId);
   }
}

void light_send_task_cb() {
   OpenQueueEntry_t*    pktToSend;
   
   // get a free packet buffer
   pktToSend = openqueue_getFreePacketBuffer(COMPONENT_LIGHT);
   if (pktToSend==NULL) {
      openserial_printError(COMPONENT_LIGHT,ERR_NO_FREE_PACKET_BUFFER,0,0);
      return;
   }
   light_format_packet(pktToSend);
   
#ifdef LIGHT_DEBUG
   openserial_printInfo(
      COMPONENT_LIGHT,
      ERR_FLOOD_SEND,
      (errorparameter_t)light_vars.seqnum,
      (errorparameter_t)light_vars.light_state
   );
#endif
       
   if ((sixtop_send(pktToSend))==E_FAIL) {
      openqueue_freePacketBuffer(pktToSend);
   }
}

port_INLINE void light_format_packet(OpenQueueEntry_t* pkt) {
   // take ownership over the packet
   pkt->owner                               = COMPONENT_LIGHT;
   pkt->creator                             = COMPONENT_LIGHT;

#ifdef LIGHT_CALCULATE_DELAY
   // TODO add light_vars.lastEventAsn into packet
#endif
   
   // fill payload
   packetfunctions_reserveHeaderSize(pkt,sizeof(light_ht));
   ((light_ht*)(pkt->payload))->type        = 0xdddd;
   ((light_ht*)(pkt->payload))->src         = idmanager_getMyShortID();
   ((light_ht*)(pkt->payload))->seqnum      = light_vars.seqnum;
   ((light_ht*)(pkt->payload))->light_state = light_vars.light_state;
}

void light_sendDone(OpenQueueEntry_t* msg, owerror_t error) {
   openqueue_freePacketBuffer(msg);
}

//== receiving

// receive a beacon packet and analyse it
void light_receive_beacon(OpenQueueEntry_t* pkt) {
   OpenQueueEntry_t* fwPkt;
   int16_t           counter;
   bool              state;
   
   // abort if not sync'ed
   if (ieee154e_isSynch() == FALSE) {
     return;
   }
   
   // acquire ownserhip over the packet
   pkt->owner = COMPONENT_LIGHT;
   
   // retrieve the counter
   counter    = pkt->l2_floodingCounter;
   
   // retrieve the state
   state      = pkt->l2_floodingState;
   
   // update my info and drop if the beacon has a more recent counter
   if (counter >= light_vars.seqnum) {
      // update my counter
      light_vars.seqnum = counter;
      
      if (light_vars.light_state != state) {
        // update my state if I am not the sensor node
        if (!light_checkMyId(SENSOR_ID)) {
          light_vars.light_state = state;
        }
        
        // if I am the sink, process the beacon (update the state)
        if (light_checkMyId(SINK_ID)) {
           light_process_packet_at_sink();
        }
      }
      return;
   }
   
   // if the packet has the same state as mine dont need to update the neighbor
   // if I am already forwarding a packet, return
   // if the packet comes from a node further from the sink, return
   if (
         (light_vars.light_state == state) ||
         (light_vars.busyForwarding == TRUE) ||
         (pkt->l2_rank >= neighbors_getMyDAGrank())
      ){
     return;
   }
   
   // if I received a beacon that is older and the node is out-of-date and closer to the sink, 
   // we should update it asap
   fwPkt = openqueue_getFreePacketBuffer(COMPONENT_LIGHT);
   if (fwPkt==NULL) {
     openserial_printError(COMPONENT_LIGHT,ERR_NO_FREE_PACKET_BUFFER,2,0);
     return;
   }
   light_format_packet(fwPkt);

   light_vars.pktToForward   = fwPkt;
   light_vars.busyForwarding = TRUE;
   light_vars.fwTimerId      = opentimers_start(
      (openrandom_get16b()&0x3f),
      TIMER_ONESHOT,
      TIME_MS,
      light_timer_fwd_cb
   );
   
#ifdef LIGHT_DEBUG
   openserial_printInfo(
      COMPONENT_LIGHT,
      ERR_FLOOD_GEN,
      (errorparameter_t)light_vars.seqnum,
      (errorparameter_t)light_vars.light_state
   );
#endif
}

// receive a data packet and analyse it
void light_receive_data(OpenQueueEntry_t* pkt) {
   OpenQueueEntry_t* fwpkt;
   uint16_t          seqnum;
   bool              light_state;
#ifdef LIGHT_CALCULATE_DELAY
   uint8_t           i;
   uint8_t           asn[5];
#endif
   
   // don't run if not synched
   if (ieee154e_isSynch() == FALSE) {
      openqueue_freePacketBuffer(pkt);
      return;
   }
   
   // take ownserhip over the packet
   pkt->owner = COMPONENT_LIGHT;
   
   // retrieve the counter
   seqnum = pkt->payload[0] | (pkt->payload[1] << 8);
   
   // retrieve the state
   light_state = (bool)pkt->payload[2];
   
#ifdef LIGHT_CALCULATE_DELAY
   // retrieve the asn from the packet
   for (i=0; i<5; i++) {
     asn[i] = pkt->payload[3+i];
   }
#endif
   
#ifdef LIGHT_DEBUG
   openserial_printInfo(
      COMPONENT_LIGHT,ERR_FLOOD_RCV,
      (errorparameter_t)seqnum,
      (errorparameter_t)light_state
   );
#endif     
   
   // free the packet
   openqueue_freePacketBuffer(pkt);
   
   // drop if we already received this packet
   if (seqnum<=light_vars.seqnum) {
#ifdef LIGHT_DEBUG
      openserial_printInfo(
         COMPONENT_LIGHT,ERR_FLOOD_DROP,
         (errorparameter_t)seqnum,
         (errorparameter_t)light_state
     );
#endif     
     return;
   }
   
   // update the seqnum 
   light_vars.seqnum = seqnum;
   
   // update the state
   light_vars.light_state = light_state;
   
#ifdef LIGHT_CALCULATE_DELAY
   // ASN retrieve the asn from the packet, store in light_vars.lastEventAsn
#endif
   
   // if I am the sink, process the message (update the state)
   if (light_checkMyId(SINK_ID)) {
      light_process_packet_at_sink();
      return;
   }
   
   if (light_vars.busyForwarding==TRUE) {
      return;
   }
   
   // if I am not the sink, let's forward
   fwpkt = openqueue_getFreePacketBuffer(COMPONENT_LIGHT);
   if (fwpkt==NULL) {
      openserial_printError(COMPONENT_LIGHT,ERR_NO_FREE_PACKET_BUFFER,1,0);
      return;
   }
   light_format_packet(fwpkt);
   
   light_vars.pktToForward   = fwpkt;
   light_vars.busyForwarding = TRUE;
   light_vars.fwTimerId      = opentimers_start(
      (openrandom_get16b()&0x3f),
      TIMER_ONESHOT,
      TIME_MS,
      light_timer_fwd_cb
   );
   
#ifdef LIGHT_DEBUG
   openserial_printInfo(
      COMPONENT_LIGHT,
      ERR_FLOOD_FW,
      (errorparameter_t)seqnum,
      (errorparameter_t)light_state
   );
#endif
}

void light_timer_fwd_cb(opentimer_id_t id) {
   if ((sixtop_send(light_vars.pktToForward))==E_FAIL) {
      openqueue_freePacketBuffer(light_vars.pktToForward);
   }
   light_vars.busyForwarding = FALSE;
   light_vars.pktToForward   = NULL;
}

//=== misc

port_INLINE bool light_get_light_state(void) {
  return light_vars.light_state;
}

port_INLINE uint16_t light_get_seqnum(void) {
  return light_vars.seqnum;
}

// check if my id is equal to addr
port_INLINE bool light_checkMyId(uint16_t addr) {
  return ((idmanager_getMyID(ADDR_64B)->addr_64b[7] == (addr & 0xff)) &&
          (idmanager_getMyID(ADDR_64B)->addr_64b[6] == (addr >> 8)));
}

//=========================== private ==========================================

void light_process_packet_at_sink() {
#ifdef LIGHT_CALCULATE_DELAY
   asn_t      event_asn;
   uint16_t   asnDiff;
#endif
   
   // switch rxlight pin high/low
   if (light_vars.light_state==TRUE) {
      debugpins_rxlight_set();
   } else {
      debugpins_rxlight_clr();
   }
   
#ifdef LIGHT_CALCULATE_DELAY
   // calculate the time difference between the asn in the packet and now
   // TODO
   
   // print
   /*
   openserial_printInfo(
      COMPONENT_LIGHT,
      ERR_FLOOD_STATE,
      (errorparameter_t)light_vars.light_state,
      (errorparameter_t)asnDiff
   );
   */
#else 
   openserial_printInfo(
      COMPONENT_LIGHT,
      ERR_FLOOD_STATE,
      (errorparameter_t)light_vars.light_state,
      (errorparameter_t)0
   );
#endif
}

