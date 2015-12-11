#ifndef __SIXTOP_LIGHT_H
#define __SIXTOP_LIGHT_H

#include "opentimers.h"

//=========================== define ==========================================

#define SIXTOP_LIGHT_SEND_MS       1
#define LUX_THRESHOLD              2000

//=========================== typedef =========================================

//=========================== variables =======================================

typedef struct {
   opentimer_id_t       cancelTimerId;  ///< periodic timer which triggers when to cancel the processing
   opentimer_id_t       sendTimerId;    ///< periodic timer which triggers when to transmit      
   int16_t              counter;        ///< incrementing counter which is written into the packet
   uint16_t             lux;
   bool                 state;
   bool                 initialized;
} sixtop_light_vars_t;

//=========================== prototypes ======================================

void sixtop_light_init();
void sixtop_light_sendDone(OpenQueueEntry_t* msg, owerror_t error);
void sixtop_light_receive(OpenQueueEntry_t* msg);
void sixtop_light_send(uint16_t lux, bool state);
void sixtop_light_initialize(bool state);
bool sixtop_light_is_initialized(void);
bool sixtop_light_state(void);

#endif
