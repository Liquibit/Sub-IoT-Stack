/*
 * Copyright (c) 2015-2021 University of Antwerp, Aloxy NV.
 *
 * This file is part of Sub-IoT.
 * See https://github.com/Sub-IoT/Sub-IoT-Stack for further info.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <string.h>

#include "hwuart.h"
#include "hwdma.h"
#include "alp_layer.h"
#include "framework_defs.h"
#include "platform_defs.h"
#include "fifo.h"
#include "scheduler.h"
#include "modem_interface.h"
#include "modules_defs.h"
#include "hal_defs.h"
#include "debug.h"
#include "errors.h"
#include "platform_defs.h"
#include "timer.h"

#include "hwsystem.h"
#include "hwatomic.h"
#include "crc.h"

#include "ng.h"
#include "crc.h"

#include "log.h"


#define RX_BUFFER_SIZE 256

#define TX_FIFO_FLUSH_CHUNK_SIZE 10 // at a baudrate of 115200 this ensures completion within 1 ms
                                    // TODO baudrate dependent

static uart_handle_t* uart;
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
static dma_handle_t* dma_rx;
static dma_handle_t* dma_tx;
static uint16_t tx_size;
static bool tx_dma_busy = false;
#ifndef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
_Static_assert(false, "FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES should be defined if FRAMEWORK_MODEM_INTERFACE_USE_DMA is used.");
#endif
#endif

#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
#define MODEM_INTERFACE_TIMEOUT            (5 * TIMER_TICKS_PER_SEC)
#define MODEM_INTERFACE_TIMEOUT_BLOCK_TIME (5 * TIMER_TICKS_PER_MINUTE)
static bool modem_interface_timeout_active;
static timer_tick_t modem_interface_timeout_start_time;
#endif

static uint8_t rx_buffer[RX_BUFFER_SIZE];
static fifo_t rx_fifo;

#if defined(FRAMEWORK_LOG_ENABLED) && defined(FRAMEWORK_MODEM_INTERFACE_LOG_ENABLED)
  #define DPRINT(...) log_print_string(__VA_ARGS__)
  #define DPRINT_DATA(...) log_print_data(__VA_ARGS__)
#else
  #define DPRINT(...)
  #define DPRINT_DATA(...)
#endif


#define SERIAL_FRAME_SYNC_BYTE 0xC0
#define SERIAL_FRAME_VERSION   0x00
#define SERIAL_FRAME_HEADER_SIZE 7
#define SERIAL_FRAME_SIZE 4
#define SERIAL_FRAME_COUNTER 2
#define SERIAL_FRAME_TYPE 3
#define SERIAL_FRAME_CRC1   5
#define SERIAL_FRAME_CRC2   6

#define MODEM_INTERFACE_TX_FIFO_SIZE 255
static uint8_t modem_interface_tx_buffer[MODEM_INTERFACE_TX_FIFO_SIZE];
static fifo_t modem_interface_tx_fifo;
static bool request_pending = false;

uint8_t header[SERIAL_FRAME_HEADER_SIZE];
static uint8_t payload_len = 0;
static uint8_t packet_up_counter = 0;
static uint8_t packet_down_counter = 0;
static pin_id_t uart_state_pin;
static pin_id_t target_uart_state_pin;
static volatile uint8_t target_uart_state_isr_count;

static bool modem_listen_uart_inited = false;
static bool parsed_header = false;

static cmd_handler_t alp_handler;
static cmd_handler_t ping_response_handler;
static cmd_handler_t logging_handler;
static target_rebooted_callback_t target_rebooted_cb;

typedef enum {
  STATE_IDLE,
  STATE_REQ_START,
  STATE_REQ_WAIT,
  STATE_REQ_BUSY,
  STATE_RESP,
  STATE_RESP_PENDING_REQ,
  STATE_REC
} state_t;

static state_t state = STATE_IDLE;

#define SWITCH_STATE(s) do { \
  state = s; \
  DPRINT("switch to %s\n", #s); \
} while(0)

static void process_rx_fifo(void *arg);
static void execute_state_machine();


/** @Brief Enable UART interface and UART interrupt
 *  @return void
 */
static void modem_interface_enable(void) 
{
  DPRINT("uart enabled @ %i",timer_get_counter_value());
  assert(uart_enable(uart));
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
  assert(dma_channel_enable(dma_rx));
  uart_start_read_bytes_via_DMA(uart, rx_buffer, RX_BUFFER_SIZE, PLATFORM_MODEM_INTERFACE_DMA_RX);
  uart_tx_interrupt_enable(uart);
#else 
  uart_rx_interrupt_enable(uart);
#endif
  modem_listen_uart_inited = true;
}

/** @Brief disables UART interface
 *  @return void
 */
static void modem_interface_disable(void) 
{
  modem_listen_uart_inited = false;
  assert(uart_disable(uart));
  uart_pull_down_rx(uart);
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
  assert(dma_channel_disable(dma_rx));
  assert(dma_channel_disable(dma_tx));
  uart_tx_interrupt_disable(uart);
#else
  uart_rx_interrupt_disable(uart);
#endif
  DPRINT("uart disabled @ %i",timer_get_counter_value());
}

/** @brief Lets receiver know that 
 *  all the data has been transfered
 *  @return void
 */
static void release_receiver()
{
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
  DPRINT("release receiver\n");
  modem_interface_disable();
  hw_gpio_clr(uart_state_pin);
#endif
}

/** @brief transmit data in fifo to UART
 *  @return void
 */
static void flush_modem_interface_tx_fifo(void *arg) 
{
  uint8_t len = fifo_get_size(&modem_interface_tx_fifo);

#ifdef HAL_UART_USE_DMA_TX
  // when using DMA we transmit the whole FIFO at once
  uint8_t buffer[MODEM_INTERFACE_TX_FIFO_SIZE];
  fifo_pop(&modem_interface_tx_fifo, buffer, len);
  uart_send_bytes(uart, buffer, len);
#elif defined(FRAMEWORK_MODEM_INTERFACE_USE_DMA)
  //Execute atomic, otherwise there is a chance that the DMA complete callback is called during execution of this code.
  // If that would happen a DMA transfer with length 0 is started which will not trigger a complete callback
  // This means that we have to fetch the data size inside the fifo inside the atomic part again
  start_atomic();
  len = fifo_get_size(&modem_interface_tx_fifo);
  if(len > 0)
  {
    //log_print_string("flush %d", tx_dma_busy);
    if(!tx_dma_busy)
    {
      assert(dma_channel_enable(dma_tx));
      dma_channel_interrupt_enable(dma_tx);
      uint8_t* buffer;
      fifo_get_continuos_raw_data(&modem_interface_tx_fifo, &buffer, &tx_size);
      uart_send_bytes_via_DMA(uart, buffer, tx_size, PLATFORM_MODEM_INTERFACE_DMA_TX);
      tx_dma_busy = true;
    }
    //To keep awake
    sched_post_task_prio(&flush_modem_interface_tx_fifo, MIN_PRIORITY, NULL);
  }
  else
  {
    request_pending = false;
    dma_channel_interrupt_disable(dma_tx);
    release_receiver();
    sched_post_task(&execute_state_machine);
  }
  end_atomic();
#else
  // only send small chunks over uart each invocation, to make sure
  // we don't interfer with critical stack timings.
  // When there is still data left in the fifo this will be rescheduled
  // with lowest prio
  uint8_t chunk[TX_FIFO_FLUSH_CHUNK_SIZE];
  if(len <= TX_FIFO_FLUSH_CHUNK_SIZE)
  {
    fifo_pop(&modem_interface_tx_fifo, chunk, len);
    uart_send_bytes(uart, chunk, len);
    request_pending = false;
    release_receiver();
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
    sched_post_task(&execute_state_machine);
#endif
  } 
  else 
  {
    fifo_pop(&modem_interface_tx_fifo, chunk, TX_FIFO_FLUSH_CHUNK_SIZE);
    uart_send_bytes(uart, chunk, TX_FIFO_FLUSH_CHUNK_SIZE);
    sched_post_task_prio(&flush_modem_interface_tx_fifo, MIN_PRIORITY, NULL);
  }
#endif
}

/** @Brief Keeps µC awake while receiving UART data
 *  @return void
 */
static void modem_listen(void* arg)
{
  if(!modem_listen_uart_inited)
  {
    modem_interface_enable();
    hw_gpio_set(uart_state_pin); //set interrupt gpio to indicate ready for data
  }
}

#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
static bool modem_interface_timeout()
{
  if(modem_interface_timeout_active)
  {
    timer_tick_t timer_value = timer_get_counter_value();
    timer_tick_t diff = timer_calculate_difference(modem_interface_timeout_start_time, timer_value);
    if(diff > MODEM_INTERFACE_TIMEOUT)
    {
#ifdef FRAMEWORK_DEBUG_ASSERT_REBOOT
      modem_interface_clear_handler();
      log_print_error_string("modem_interface timeout occurred in state %d", state);
      return true;
#else
      log_print_string("modem_interface timeout occurred in state %d", state);
      if(timer_value < MODEM_INTERFACE_TIMEOUT_BLOCK_TIME)
      {
        modem_interface_clear_handler();
        return true;
      }
      assert(false);
#endif
    }
  }
  else
  {
    modem_interface_timeout_start_time = timer_get_counter_value();
    modem_interface_timeout_active = true;
  }
  return false;
}

static inline void clear_modem_interface_timeout()
{
  modem_interface_timeout_active = false;
}
#endif

/** @Brief Handles the different states of the modem interrupt line handshake mechanism
 *  @return void
 */
static void execute_state_machine()
{
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
    switch (state) {
    case STATE_REC:
    {
      if(hw_gpio_get_in(target_uart_state_pin))
      {
        if(!modem_interface_timeout())
        {
          sched_post_task_prio(&execute_state_machine, MIN_PRIORITY, NULL);
        }
        else
        {
          return;
        }
        break;
      }
      clear_modem_interface_timeout();
      target_uart_state_isr_count = 0;
      SWITCH_STATE(STATE_RESP);
      __attribute__ ((fallthrough));
      //Intentional fall through
    }
    case STATE_RESP:
    {
      // response period completed, process the request
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
      size_t received_bytes = uart_stop_read_bytes_via_DMA(uart);
      fifo_init_filled(&rx_fifo, rx_buffer, received_bytes, RX_BUFFER_SIZE);
#endif
      sched_post_task(&process_rx_fifo);
      if(request_pending) {
        SWITCH_STATE(STATE_RESP_PENDING_REQ);
        sched_post_task(&execute_state_machine);
      } else {
        SWITCH_STATE(STATE_IDLE);
        hw_gpio_clr(uart_state_pin);
        sched_cancel_task(&modem_listen);
        modem_interface_disable();
      }
      break;
    }
    case STATE_IDLE:
      if(hw_gpio_get_in(target_uart_state_pin)) {
        // wake-up requested
        target_uart_state_isr_count = 0;
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
        if(fifo_get_size(&rx_fifo) == 0)
        {
          clear_modem_interface_timeout();
#endif
          SWITCH_STATE(STATE_REC);
          modem_listen(NULL);
          sched_post_task_prio(&execute_state_machine, MIN_PRIORITY, NULL);
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
        }
        else
        {
          // We are still processing previous data, wait until fifo is empty
          if(!modem_interface_timeout())
          {
            sched_post_task_prio(&execute_state_machine, MIN_PRIORITY, NULL);
          }
        }
#endif
        break;
      } else if(request_pending) { //check if we are really requesting a start
        SWITCH_STATE(STATE_REQ_START);
        // fall-through to STATE_REQ_START!
      } else
      {
        break;
      }
    case STATE_REQ_START:
      sched_cancel_task(&modem_listen);
      SWITCH_STATE(STATE_REQ_WAIT);
      hw_gpio_set(uart_state_pin); // wake-up receiver
      DPRINT("wake-up receiver\n");
      sched_post_task(&execute_state_machine); // reschedule again to prevent sleeoping
      // in principle we could go to sleep but this will cause pin to float, this can be improved later
      break;
    case STATE_REQ_WAIT:
      if(hw_gpio_get_in(target_uart_state_pin)) {
        target_uart_state_isr_count = 0;
        clear_modem_interface_timeout();
        // receiver active
        SWITCH_STATE(STATE_REQ_BUSY);
        // fall-through to STATE_REQ_BUSY!
      } else {
        if(!modem_interface_timeout())
        {
          sched_post_task_prio(&execute_state_machine, MIN_PRIORITY, NULL);
        }
        // in principle we could go to sleep but this will cause pin to float, this can be improved later
        break;
      }
    case STATE_REQ_BUSY:
      if(request_pending) {
        //if receiver reacts fast this code is executed twice (once due to the fall through of the previous state and once scheduled by the uart_int_cb)
        //To be sure that modem_interface_enable is not called the second time when the transfer is already busy it is guaranteed that the code is only executed once.
        if(!sched_is_scheduled(&flush_modem_interface_tx_fifo) 
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
        || !tx_dma_busy
#endif
        )
        {
          clear_modem_interface_timeout();
          modem_interface_enable();
          sched_post_task_prio(&flush_modem_interface_tx_fifo, MIN_PRIORITY, NULL);
        }
      } else if ((!hw_gpio_get_in(target_uart_state_pin) || !uart_get_rx_port_state(uart))
                 || (hw_gpio_get_in(target_uart_state_pin) && target_uart_state_isr_count > 1)) // AL-2405: We have missed going down of target_uart_state_pin
      {
          clear_modem_interface_timeout();
        target_uart_state_isr_count = 0;
        SWITCH_STATE(STATE_IDLE);
        sched_post_task(&execute_state_machine);
      } else{
        if(!modem_interface_timeout())
        {
          sched_post_task_prio(&execute_state_machine, MIN_PRIORITY, NULL);
        }
        //keep active until target reacts
      }
      break;
    case STATE_RESP_PENDING_REQ:
      assert(request_pending);
      // response period completed, initiate pending request by switching to REQ_START
      assert(!hw_gpio_get_in(target_uart_state_pin));
      hw_gpio_clr(uart_state_pin);
      SWITCH_STATE(STATE_REQ_START);
      sched_post_task(&execute_state_machine);
      break;
    default:
      DPRINT("unexpected state %i\n", state);
      assert(false);
  }
#endif
}

/** @Brief Check package counter and crc
 *  @return void
 */
static bool verify_payload(fifo_t* bytes, uint8_t* frame_header)
{
  static uint8_t payload[RX_BUFFER_SIZE - SERIAL_FRAME_HEADER_SIZE]; // statically allocated so this does not end up on stack
  fifo_peek(bytes, payload, 0, frame_header[SERIAL_FRAME_SIZE]);

  //check for missing packages
  packet_down_counter++;
  if(frame_header[SERIAL_FRAME_COUNTER]!=packet_down_counter)
  {
    //TODO consequence? (save total missing packages?)
    log_print_string("!!! missed packages: %i",(frame_header[SERIAL_FRAME_COUNTER]-packet_down_counter));
    packet_down_counter=frame_header[SERIAL_FRAME_COUNTER]; //reset package counter
  }

  DPRINT("RX HEADER: ");
  DPRINT_DATA(frame_header, SERIAL_FRAME_HEADER_SIZE);
  DPRINT("RX PAYLOAD: ");
  DPRINT_DATA(payload, frame_header[SERIAL_FRAME_SIZE]);

  uint16_t calculated_crc = crc_calculate(payload, frame_header[SERIAL_FRAME_SIZE]);
 
  if(frame_header[SERIAL_FRAME_CRC1]!=((calculated_crc >> 8) & 0x00FF) || frame_header[SERIAL_FRAME_CRC2]!=(calculated_crc & 0x00FF))
  {
    //TODO consequence? (request repeat?)
    log_print_string("CRC incorrect!");
    return false;
  }
  else
    return true;
}

/**
 * @brief reset modem interface because of error or reset of the other party
 * 
 */
void modem_interface_clear_handler() {
    modem_interface_disable();

    //clear RX
    parsed_header = false;
    payload_len = 0;
    fifo_clear(&rx_fifo);

    //clear TX
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
    tx_dma_busy = false;
    tx_size = 0;
#endif
    target_uart_state_isr_count = 0;
    request_pending = false;
    fifo_clear(&modem_interface_tx_fifo);
    //AL-2305 be sure to clear request pin
    hw_gpio_clr(uart_state_pin);
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
    modem_interface_timeout_active = false;
#endif
    SWITCH_STATE(STATE_IDLE);
}


static void uart_error_callback(uart_error_t error) {
    log_print_string("UART ERROR %i", error);
    if(error == UART_OVERRUN_ERROR) {
      parsed_header = false;
      payload_len = 0;
      fifo_clear(&rx_fifo);
    }
}

/** @Brief Processes received uart data
 * 1) Search for sync bytes (always)
 * 2) get header size and parse header
 * 3) Wait for correct # of bytes (length present in header)
 * 4) Execute crc check and check message counter
 * 5) send to corresponding service (alp, ping service, log service)
 *  @return void
 */
static void process_rx_fifo(void *arg) 
{
  if(!parsed_header) 
  {
    if(fifo_get_size(&rx_fifo) > SERIAL_FRAME_HEADER_SIZE) 
    {
        fifo_peek(&rx_fifo, header, 0, SERIAL_FRAME_HEADER_SIZE);

        if(header[0] != SERIAL_FRAME_SYNC_BYTE || header[1] != SERIAL_FRAME_VERSION) 
        {
          fifo_skip(&rx_fifo, 1);
          DPRINT("skip");
          parsed_header = false;
          payload_len = 0;
          if(fifo_get_size(&rx_fifo) > SERIAL_FRAME_HEADER_SIZE)
            sched_post_task(&process_rx_fifo);
          return;
        }
        parsed_header = true;
        fifo_skip(&rx_fifo, SERIAL_FRAME_HEADER_SIZE);
        payload_len = header[SERIAL_FRAME_SIZE];
        DPRINT("UART RX, payload size = %i", payload_len);
        sched_post_task(&process_rx_fifo);
    }
  }
  else 
  {
    if(fifo_get_size(&rx_fifo) < payload_len) {
      return;
    }
    // payload complete, start parsing
    // rx_fifo can be bigger than the current serial packet, init a subview fifo
    // which is restricted to payload_len so we can't parse past this packet.
    fifo_t payload_fifo;
    fifo_init_subview(&payload_fifo, &rx_fifo, 0, payload_len);
  
    if(verify_payload(&payload_fifo,header))
    {
      if(header[SERIAL_FRAME_TYPE]==SERIAL_MESSAGE_TYPE_ALP_DATA && alp_handler != NULL)
        alp_handler(&payload_fifo);
      else if (header[SERIAL_FRAME_TYPE]==SERIAL_MESSAGE_TYPE_PING_RESPONSE  && ping_response_handler != NULL)
        ping_response_handler(&payload_fifo);
      else if (header[SERIAL_FRAME_TYPE]==SERIAL_MESSAGE_TYPE_LOGGING && logging_handler != NULL)
        logging_handler(&payload_fifo);
      else if (header[SERIAL_FRAME_TYPE]==SERIAL_MESSAGE_TYPE_PING_REQUEST)
      {
#ifdef MODULE_ALP
        // free all alp commands
        alp_layer_free_commands();
#endif
        uint8_t ping_reply[1]={0x02};
        fifo_skip(&payload_fifo,1);
        modem_interface_transfer_bytes(ping_reply,1,SERIAL_MESSAGE_TYPE_PING_RESPONSE);
      }
      else if(header[SERIAL_FRAME_TYPE]==SERIAL_MESSAGE_TYPE_REBOOTED)
      {
        uint8_t reboot_reason;
        fifo_pop(&payload_fifo, &reboot_reason, 1);
        DPRINT("target rebooted, reason=%i\n", reboot_reason);
        if(target_rebooted_cb)
          target_rebooted_cb(reboot_reason);
      }
      else
      {
        fifo_skip(&payload_fifo, payload_len);
        DPRINT("!!!FRAME TYPE NOT IMPLEMENTED");
      }
      fifo_skip(&rx_fifo, payload_len - fifo_get_size(&payload_fifo)); // pop parsed bytes from original fifo
    }
    else 
    {
        DPRINT("!!!PAYLOAD DATA INCORRECT");
    }
      
    payload_len = 0;
    parsed_header = false;
    if(fifo_get_size(&rx_fifo) > SERIAL_FRAME_HEADER_SIZE)
      sched_post_task(&process_rx_fifo);
  }
}

#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
static void uart_tx_cb()
{
  tx_dma_busy = false;
  fifo_skip(&modem_interface_tx_fifo, tx_size);
}
#else
/** @Brief put received UART data in fifo
 *  @return void
 */
static void uart_rx_callback(uint8_t data)
{
    error_t err;
    start_atomic();
    err = fifo_put(&rx_fifo, &data, 1);
    assert(err == SUCCESS);
    end_atomic();

#ifndef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
    sched_post_task(&process_rx_fifo);
#endif
}
#endif

/** @Brief Processes events on UART interrupt line
 *  @return void
 */
static void uart_int_cb(void *arg)
{
  target_uart_state_isr_count++;
  // do not read GPIO level here in interrupt context (GPIO clock might not be enabled yet), execute state machine instead
  sched_post_task(&execute_state_machine);
}

static void modem_interface_set_rx_interrupt_callback(uart_rx_inthandler_t uart_rx_cb) {
#ifdef PLATFORM_USE_USB_CDC
	cdc_set_rx_interrupt_callback(uart_rx_cb);
#else
  uart_set_rx_interrupt_callback(uart, uart_rx_cb);
#endif
}

static void modem_interface_set_error_callback(uart_error_handler_t uart_error_cb) {
    uart_set_error_callback(uart, uart_error_cb);
}

void modem_interface_init(uint8_t idx, uint32_t baudrate, pin_id_t uart_state_pin_id, pin_id_t target_uart_state_pin_id)
{
  fifo_init(&modem_interface_tx_fifo, modem_interface_tx_buffer, MODEM_INTERFACE_TX_FIFO_SIZE);
  sched_register_task(&flush_modem_interface_tx_fifo);
  sched_register_task(&execute_state_machine);
  sched_register_task(&process_rx_fifo);
  state = STATE_IDLE;
  uart_state_pin=uart_state_pin_id;
  target_uart_state_pin=target_uart_state_pin_id;

  uart = uart_init(idx, baudrate,0);
  DPRINT("uart initialized");
  
  fifo_init(&rx_fifo, rx_buffer, sizeof(rx_buffer));
#ifdef FRAMEWORK_MODEM_INTERFACE_USE_DMA
  dma_rx = dma_channel_init(PLATFORM_MODEM_INTERFACE_DMA_RX);
  dma_tx = dma_channel_init(PLATFORM_MODEM_INTERFACE_DMA_TX);
  uart_set_tx_interrupt_callback(uart, &uart_tx_cb);
#else
  modem_interface_set_rx_interrupt_callback(&uart_rx_callback);
#endif
  modem_interface_set_error_callback(&uart_error_callback);

#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
  assert(sched_register_task(&modem_listen) == SUCCESS);
  DPRINT("using interrupt lines");
  assert(hw_gpio_configure_interrupt(target_uart_state_pin, GPIO_RISING_EDGE | GPIO_FALLING_EDGE, &uart_int_cb, NULL) == SUCCESS);
  assert(hw_gpio_enable_interrupt(target_uart_state_pin) == SUCCESS);
  if(hw_gpio_get_in(target_uart_state_pin))
  {
    sched_post_task(&modem_listen);
    DPRINT("Ready to receive (boot) @ %i",timer_get_counter_value());
  }
#endif

// When not using interrupt lines we keep uart enabled so we can use RX IRQ.
// If the platform has interrupt lines the UART should be re-enabled when handling the modem interrupt
#ifndef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
  modem_interface_enable();
#endif

  uint8_t reboot_reason = (uint8_t)hw_system_reboot_reason();
  log_print_error_string("rebooted with reason %i", reboot_reason);
#ifndef PLATFORM_NO_REBOOT_REASON_TRANSMIT
  modem_interface_transfer_bytes(&reboot_reason, 1, SERIAL_MESSAGE_TYPE_REBOOTED);
#endif
}

error_t modem_interface_transfer_bytes(uint8_t* bytes, uint8_t length, serial_message_type_t type) 
{
  error_t result;
  uint8_t frame_header[SERIAL_FRAME_HEADER_SIZE];
  uint16_t crc=crc_calculate(bytes,length);

  packet_up_counter++;
  frame_header[0] = SERIAL_FRAME_SYNC_BYTE;
  frame_header[1] = SERIAL_FRAME_VERSION;

  frame_header[SERIAL_FRAME_COUNTER] = packet_up_counter;
  frame_header[SERIAL_FRAME_TYPE] = type;
  frame_header[SERIAL_FRAME_SIZE] = length;
  frame_header[SERIAL_FRAME_CRC1] = (crc >> 8) & 0x00FF;
  frame_header[SERIAL_FRAME_CRC2] = crc & 0x00FF;

  DPRINT("TX HEADER:");
  DPRINT_DATA(frame_header, SERIAL_FRAME_HEADER_SIZE);
  DPRINT("TX PAYLOAD:");
  DPRINT_DATA(bytes, length);
   
  start_atomic();
  if((sizeof(modem_interface_tx_buffer) - fifo_get_size(&modem_interface_tx_fifo)) >= (uint8_t) (SERIAL_FRAME_HEADER_SIZE + length))
  {
    request_pending = true;
    fifo_put(&modem_interface_tx_fifo, (uint8_t*) &frame_header, SERIAL_FRAME_HEADER_SIZE);
    fifo_put(&modem_interface_tx_fifo, bytes, length);

#ifdef FRAMEWORK_MODEM_INTERFACE_USE_INTERRUPT_LINES
    sched_post_task_prio(&execute_state_machine, MIN_PRIORITY, NULL);
#else
    sched_post_task_prio(&flush_modem_interface_tx_fifo, MIN_PRIORITY, NULL); // state machine is not used when not using interrupt lines
#endif 
    result = SUCCESS;
  }
  else
  {
    result = -ENOMEM;
  }
  end_atomic();
  return result;
}

error_t modem_interface_transfer(char* string) {
  return modem_interface_transfer_bytes((uint8_t*) string, strnlen(string, 100), SERIAL_MESSAGE_TYPE_LOGGING); 
}

void modem_interface_register_handler(cmd_handler_t cmd_handler, serial_message_type_t type)
{
  switch(type) {
    case SERIAL_MESSAGE_TYPE_ALP_DATA:
      alp_handler = cmd_handler;
      break;
    case SERIAL_MESSAGE_TYPE_PING_RESPONSE:
      ping_response_handler = cmd_handler;
      break;
    case SERIAL_MESSAGE_TYPE_LOGGING:
      logging_handler = cmd_handler;
      break;
    default:
      DPRINT("Modem interface callback not implemented");
  }
}

void modem_interface_unregister_handler(serial_message_type_t type)
{
  modem_interface_register_handler(NULL, type);
}

void modem_interface_set_target_rebooted_callback(target_rebooted_callback_t cb)
{
  target_rebooted_cb = cb;
}
