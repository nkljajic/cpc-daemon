/***************************************************************************//**
 * @file
 * @brief Co-Processor Communication Protocol(CPC) - System Endpoint
 * @version 3.2.0
 *******************************************************************************
 * # License
 * <b>Copyright 2021 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "server_internal.h"
#include "system.h"

#include "log.h"
#include "server.h"
#include "core.h"

/***************************************************************************//**
 * Used to return the size of a system command buffer, primarily to pass to
 * sl_cpc_write.
 ******************************************************************************/
#define SIZEOF_SYSTEM_COMMAND(command) (sizeof(sl_cpc_system_cmd_t) + command->length)

/***************************************************************************//**
 * This variable holds the sequence number for the next command that will be
 * issued. It is incremented and wrapped around each time a command is sent.
 ******************************************************************************/
static uint8_t next_command_seq = 0;

static sl_slist_node_t *commands;
static sl_slist_node_t *retries;
static sl_slist_node_t *commands_in_error;

typedef struct {
  sl_slist_node_t node;
  sl_cpc_system_unsolicited_status_callback_t callback;
}prop_last_status_callback_list_item_t;

static sl_slist_node_t *prop_last_status_callbacks;

static void on_unsolicited(uint8_t endpoint_id, const void* data, size_t data_len);
static void on_reply(uint8_t endpoint_id, void *arg, void *answer, uint32_t answer_lenght);
static void write_command(sl_cpc_system_command_handle_t *command_handle);

void sl_cpc_system_init(void)
{
  sl_slist_init(&commands);
  sl_slist_init(&retries);
  sl_slist_init(&commands_in_error);
  sl_slist_init(&prop_last_status_callbacks);

  //Open system endpoint
  core_open_endpoint(SL_CPC_ENDPOINT_SYSTEM,
                     SL_CPC_OPEN_ENDPOINT_FLAG_IFRAME_DISABLE | SL_CPC_OPEN_ENDPOINT_FLAG_UFRAME_ENABLE,
                     1);

  core_set_endpoint_option(SL_CPC_ENDPOINT_SYSTEM,
                           SL_CPC_ENDPOINT_ON_FINAL,
                           on_reply);

  core_set_endpoint_option(SL_CPC_ENDPOINT_SYSTEM,
                           SL_CPC_ENDPOINT_ON_UFRAME_RECEIVE,
                           on_unsolicited);
}

void sl_cpc_system_register_unsolicited_prop_last_status_callback(sl_cpc_system_unsolicited_status_callback_t callback)
{
  prop_last_status_callback_list_item_t* item = malloc(sizeof(prop_last_status_callback_list_item_t));

  item->callback = callback;

  sl_slist_push_back(&prop_last_status_callbacks, &item->node);
}

/***************************************************************************//**
 * Send no-operation command query
 ******************************************************************************/
void sl_cpc_system_cmd_noop(sl_cpc_system_noop_cmd_callback_t on_noop_reply,
                            uint8_t retry_count_max,
                            uint32_t retry_timeout_us)
{
  sl_cpc_system_command_handle_t *command_handle;

  /* Malloc the command handle and the command buffer */
  {
    command_handle = malloc(sizeof(sl_cpc_system_command_handle_t));
    command_handle->command = malloc(sizeof(sl_cpc_system_cmd_t)); //noop had nothing in the 'payload field'
  }

  /* Fill the command handle */
  {
    command_handle->on_final = (void *)on_noop_reply;
    command_handle->retry_count = retry_count_max;
    command_handle->retry_timeout_us = retry_timeout_us;
    command_handle->error_status = SL_STATUS_OK;
    command_handle->command_seq = next_command_seq++;
  }

  /* Fill the system endpoint command buffer */
  {
    sl_cpc_system_cmd_t *tx_command = command_handle->command;

    tx_command->command_id = CMD_SYSTEM_NOOP;
    tx_command->command_seq = command_handle->command_seq;
    tx_command->length = 0;
  }

  write_command(command_handle);

  TRACE_SYSTEM("NOOP (id #%u) sent", CMD_SYSTEM_NOOP);
}

/***************************************************************************//**
 * Send a reset query
 ******************************************************************************/
void sl_cpc_system_cmd_reset(sl_cpc_system_reset_cmd_callback_t on_reset_reply,
                             uint8_t retry_count_max,
                             uint32_t retry_timeout_us)
{
  sl_cpc_system_command_handle_t *command_handle;

  /* Malloc the command handle and the command buffer */
  {
    command_handle = malloc(sizeof(sl_cpc_system_command_handle_t));
    command_handle->command = malloc(sizeof(sl_cpc_system_cmd_t)); //reset had nothing in the 'payload field'
  }

  /* Fill the command handle */
  {
    command_handle->on_final = (void *)on_reset_reply;
    command_handle->retry_count = retry_count_max;
    command_handle->retry_timeout_us = retry_timeout_us;
    command_handle->error_status = SL_STATUS_OK;
    command_handle->command_seq = next_command_seq++;
  }

  /* Fill the system endpoint command buffer */
  {
    sl_cpc_system_cmd_t *tx_command = command_handle->command;

    tx_command->command_id = CMD_SYSTEM_RESET;
    tx_command->command_seq = command_handle->command_seq;
    tx_command->length = 0;
  }

  write_command(command_handle);

  TRACE_SYSTEM("reset (id #%u) sent", CMD_SYSTEM_RESET);
}

/***************************************************************************//**
 * Send a property-get query
 ******************************************************************************/
void sl_cpc_system_cmd_property_get(sl_cpc_system_property_get_set_cmd_callback_t on_property_get_reply,
                                    sl_cpc_property_id_t property_id,
                                    uint8_t retry_count_max,
                                    uint32_t retry_timeout_us)
{
  sl_cpc_system_command_handle_t *command_handle;

  /* Malloc the command handle and the command buffer */
  {
    const size_t property_get_buffer_size = sizeof(sl_cpc_system_cmd_t) + sizeof(sl_cpc_property_id_t);

    command_handle = malloc(sizeof(sl_cpc_system_command_handle_t));
    command_handle->command = malloc(property_get_buffer_size); //property-get has the property id as payload
  }

  /* Fill the command handle */
  {
    command_handle->on_final = (void *)on_property_get_reply;
    command_handle->retry_count = retry_count_max;
    command_handle->retry_timeout_us = retry_timeout_us;
    command_handle->error_status = SL_STATUS_OK;
    command_handle->command_seq = next_command_seq++;
  }

  /* Fill the system endpoint command buffer */
  {
    sl_cpc_system_cmd_t *tx_command = command_handle->command;
    sl_cpc_system_property_cmd_t *tx_property_command = (sl_cpc_system_property_cmd_t *) tx_command->payload;;

    tx_command->command_id = CMD_SYSTEM_PROP_VALUE_GET;
    tx_command->command_seq = command_handle->command_seq;
    tx_property_command->property_id = cpu_to_le32(property_id);
    tx_command->length = sizeof(sl_cpc_property_id_t);
  }

  write_command(command_handle);

  TRACE_SYSTEM("property-get (id #%u) sent with property #%u", CMD_SYSTEM_PROP_VALUE_GET, property_id);
}

/***************************************************************************//**
 * Send a property-set query
 ******************************************************************************/
void sl_cpc_system_cmd_property_set(sl_cpc_system_property_get_set_cmd_callback_t on_property_set_reply,
                                    uint8_t retry_count_max,
                                    uint32_t retry_timeout_us,
                                    sl_cpc_property_id_t property_id,
                                    const void *value,
                                    size_t value_length)
{
  sl_cpc_system_command_handle_t *command_handle;

  {
    const size_t property_get_buffer_size = sizeof(sl_cpc_system_cmd_t) + sizeof(sl_cpc_property_id_t) + value_length;

    command_handle = malloc(sizeof(sl_cpc_system_command_handle_t));
    command_handle->command = malloc(property_get_buffer_size); //property-get has the property id as payload
  }

  /* Fill the command handle */
  {
    command_handle->on_final = (void *)on_property_set_reply;
    command_handle->retry_count = retry_count_max;
    command_handle->retry_timeout_us = retry_timeout_us;
    command_handle->error_status = SL_STATUS_OK;
    command_handle->command_seq = next_command_seq++;
  }

  /* Fill the system endpoint command buffer */
  {
    sl_cpc_system_cmd_t *tx_command = command_handle->command;
    sl_cpc_system_property_cmd_t *tx_property_command = (sl_cpc_system_property_cmd_t *) tx_command->payload;;

    tx_command->command_id = CMD_SYSTEM_PROP_VALUE_SET;
    tx_command->command_seq = command_handle->command_seq;
    tx_property_command->property_id = cpu_to_le32(property_id);

    /* Adapt the property value in function of the endianess of the system.
     * We make the assumption here that if a property's length value is 2, 4 or 8 then
     * we wanted to send a property value that was a u/int16_t, a u/int32_t or a u/int64_t
     * respectively to begin with. System endpoint protocol doesn't have any other properties that have
     * length other than those anyway (plus then unit 1 byte length, which doesn't need endianness
     * awareness anyway). */
    {
      switch (value_length) {
        case 0:
          FATAL("Can't send a property-set request with value of length 0");
          break;

        case 1:
          memcpy(tx_property_command->payload, value, value_length);
          break;

        case 2:
        {
          uint16_t le16 = cpu_to_le16p((uint16_t*)value);
          memcpy(tx_property_command->payload, &le16, 2);
        }
        break;

        case 4:
        {
          uint32_t le32 = cpu_to_le32p((uint32_t*)value);
          memcpy(tx_property_command->payload, &le32, 4);
        }
        break;

        case 8:
        {
          uint64_t le64 = cpu_to_le64p((uint64_t*)value);
          memcpy(tx_property_command->payload, &le64, 8);
        }
        break;

        default:
          memcpy(tx_property_command->payload, value, value_length);
          break;
      }
    }

    tx_command->length = (uint8_t)(sizeof(sl_cpc_property_id_t) + value_length);
  }

  write_command(command_handle);

  TRACE_SYSTEM("property-set (id #%u) sent with property #%u", CMD_SYSTEM_PROP_VALUE_SET, property_id);
}

/***************************************************************************//**
 * Handle no-op from SECONDARY:
 *   This functions is called when a no-op command is received from the SECONDARY.
 *   The SECONDARY will send back a no-op in response to the one sent by the PRIMARY.
 ******************************************************************************/
static void on_final_noop(sl_cpc_system_command_handle_t *command_handle,
                          sl_cpc_system_cmd_t *reply)
{
  (void) reply;

  TRACE_SYSTEM("on_final_noop()");

  ((sl_cpc_system_noop_cmd_callback_t)command_handle->on_final)(command_handle,
                                                                command_handle->error_status);
}

/***************************************************************************//**
 * Handle reset from SECONDARY:
 *   This functions is called when a reset command is received from the SECONDARY.
 *   The SECONDARY will send back a reset in response to the one sent by the PRIMARY.
 ******************************************************************************/
static void on_final_reset(sl_cpc_system_command_handle_t * command_handle,
                           sl_cpc_system_cmd_t * reply)
{
  TRACE_SYSTEM("on_final_reset()");

  // Deal with endianness of the returned status since its a 32bit value.
  sl_cpc_system_status_t reset_status_le = *((sl_cpc_system_status_t *)(reply->payload));
  sl_cpc_system_status_t reset_status_cpu = le32_to_cpu(reset_status_le);

  ((sl_cpc_system_reset_cmd_callback_t)command_handle->on_final)(command_handle,
                                                                 command_handle->error_status,
                                                                 reset_status_cpu);
}

/***************************************************************************//**
 * Handle property-is from SECONDARY:
 *   This functions is called when a property-is command is received from the SECONDARY.
 *   The SECONDARY emits a property-is in response to a property-get/set.
 ******************************************************************************/
static void on_final_property_is(sl_cpc_system_command_handle_t * command_handle,
                                 sl_cpc_system_cmd_t * reply)
{
  sl_cpc_system_property_cmd_t *property_cmd = (sl_cpc_system_property_cmd_t*)reply->payload;
  sl_cpc_system_property_get_set_cmd_callback_t callback = (sl_cpc_system_property_get_set_cmd_callback_t)command_handle->on_final;

  /* Deal with endianness of the returned property-id since its a 32bit value. */
  sl_cpc_property_id_t property_id_le = property_cmd->property_id;
  sl_cpc_property_id_t property_id_cpu = le32_to_cpu(property_id_le);

  size_t value_length = reply->length - sizeof(sl_cpc_system_property_cmd_t);

  callback(command_handle,
           property_id_cpu,
           property_cmd->payload,
           value_length,
           command_handle->error_status);
}

/***************************************************************************//**
 * This function is called by CPC core when uframe/poll is received
 ******************************************************************************/
static void on_reply(uint8_t endpoint_id,
                     void *arg,
                     void *answer,
                     uint32_t answer_lenght)
{
  sl_cpc_system_command_handle_t *command_handle;
  sl_cpc_system_cmd_t *reply = (sl_cpc_system_cmd_t *)answer;

  (void)endpoint_id;
  (void)arg;
  (void)answer_lenght;

  TRACE_SYSTEM("on_reply()");

  FATAL_ON(reply->length != answer_lenght - sizeof(sl_cpc_system_cmd_t));

  /* Go through the list of pending requests to find the one for which this reply applies */
  SL_SLIST_FOR_EACH_ENTRY(commands, command_handle, sl_cpc_system_command_handle_t, node_commands) {
    if (command_handle->command_seq == reply->command_seq) {
      /* Stop and close the retransmit timer */
      {
        epoll_unregister(&command_handle->re_transmit_timer_private_data);

        close(command_handle->re_transmit_timer_private_data.file_descriptor);
      }

      /* Call the appropriate callback */
      switch (reply->command_id) {
        case CMD_SYSTEM_NOOP:
          on_final_noop(command_handle, reply);
          break;

        case CMD_SYSTEM_RESET:
          on_final_reset(command_handle, reply);
          break;

        case CMD_SYSTEM_PROP_VALUE_IS:
          on_final_property_is(command_handle, reply);
          break;

        case CMD_SYSTEM_PROP_VALUE_GET:
        case CMD_SYSTEM_PROP_VALUE_SET:
          FATAL("its the primary who sends those");
          break;

        default:
          FATAL("system endpoint command id not recognized");
          break;
      }

      /* Cleanup this command now that it's been serviced */
      {
        sl_slist_remove(&commands, &command_handle->node_commands);

        free(command_handle->command);
        free(command_handle);
      }

      return;
    }
  }
}

static void on_unsolicited(uint8_t endpoint_id, const void* data, size_t data_len)
{
  (void) endpoint_id;
  (void) data;
  (void) data_len;

  TRACE_SYSTEM("Unsolicited received");

  sl_cpc_system_cmd_t *reply = (sl_cpc_system_cmd_t *)data;

  FATAL_ON(reply->length != data_len - sizeof(sl_cpc_system_cmd_t));

  if (reply->command_id == CMD_SYSTEM_PROP_VALUE_IS) {
    sl_cpc_system_property_cmd_t *property = (sl_cpc_system_property_cmd_t*) reply->payload;

    if (property->property_id == PROP_LAST_STATUS) {
      prop_last_status_callback_list_item_t *item;

      SL_SLIST_FOR_EACH_ENTRY(prop_last_status_callbacks, item, prop_last_status_callback_list_item_t, node) {
        sl_cpc_system_status_t* status = (sl_cpc_system_status_t*) property->payload;

        item->callback(*status);
      }
    }
  }
}

/***************************************************************************//**
 * System endpoint timer expire callback
 ******************************************************************************/
static void on_timer_expired(epoll_private_data_t *private_data)
{
  int timer_fd = private_data->file_descriptor;
  sl_cpc_system_command_handle_t *command_handle = container_of(private_data,
                                                                sl_cpc_system_command_handle_t,
                                                                re_transmit_timer_private_data);

  /* Ack the timer */
  {
    uint64_t expiration;
    ssize_t retval;

    retval = read(timer_fd, &expiration, sizeof(expiration));

    FATAL_SYSCALL_ON(retval < 0);

    FATAL_ON(retval != sizeof(expiration));

    WARN_ON(expiration != 1); /* we missed a timeout*/
  }

  if (command_handle->retry_count > 0) {
    TRACE_SYSTEM("Command ID %u retry", command_handle->command->command_id);

    command_handle->retry_count--;

    command_handle->error_status = SL_STATUS_IN_PROGRESS; //at least one timer retry occurred

    core_write(SL_CPC_ENDPOINT_SYSTEM,
               (void *)command_handle->command,
               (uint16_t)SIZEOF_SYSTEM_COMMAND(command_handle->command),
               SL_CPC_FLAG_UNNUMBERED_POLL);
  } else {
    TRACE_SYSTEM("Command ID %u timeout", command_handle->command->command_id);

    /* Stop and close the timer */
    {
      epoll_unregister(&command_handle->re_transmit_timer_private_data);

      close(command_handle->re_transmit_timer_private_data.file_descriptor);
    }

    sl_slist_remove(&commands, &command_handle->node_commands);

    command_handle->error_status = SL_STATUS_TIMEOUT; //This will be propagated when calling the callbacks

    switch (command_handle->command->command_id) {
      case CMD_SYSTEM_NOOP:
        ((sl_cpc_system_noop_cmd_callback_t)command_handle->on_final)(command_handle, command_handle->error_status);
        break;

      case CMD_SYSTEM_RESET:
        ((sl_cpc_system_reset_cmd_callback_t)command_handle->on_final)(command_handle, command_handle->error_status, STATUS_FAILURE);
        break;

      case CMD_SYSTEM_PROP_VALUE_GET:
      case CMD_SYSTEM_PROP_VALUE_SET:
      {
        sl_cpc_system_property_cmd_t *tx_property_command = (sl_cpc_system_property_cmd_t *)command_handle->command->payload;

        ((sl_cpc_system_property_get_set_cmd_callback_t) command_handle->on_final)(command_handle,
                                                                                   tx_property_command->property_id,
                                                                                   NULL,
                                                                                   0,
                                                                                   command_handle->error_status);
      }
      break;

      case CMD_SYSTEM_PROP_VALUE_IS: //fall through
      default:
        BUG("Illegal switch");
        break;
    }

    /* Free the command handle and its buffer */
    {
      free(command_handle->command);
      free(command_handle);
    }
  }
}

/***************************************************************************//**
 * Write command on endpoint
 ******************************************************************************/
static void write_command(sl_cpc_system_command_handle_t *command_handle)
{
  int timer_fd;

  sl_slist_push_back(&commands, &command_handle->node_commands);

  core_write(SL_CPC_ENDPOINT_SYSTEM,
             (void *)command_handle->command,
             (uint16_t)SIZEOF_SYSTEM_COMMAND(command_handle->command),
             SL_CPC_FLAG_UNNUMBERED_POLL);

  /* Setup timeout timer.*/
  {
    const struct itimerspec timeout = { .it_interval = { .tv_sec = 0, .tv_nsec = (long int)command_handle->retry_timeout_us * 1000 },
                                        .it_value    = { .tv_sec = 0, .tv_nsec = (long int)command_handle->retry_timeout_us * 1000 } };

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

    FATAL_SYSCALL_ON(timer_fd < 0);

    int ret = timerfd_settime(timer_fd,
                              0,
                              &timeout,
                              NULL);

    FATAL_SYSCALL_ON(ret < 0);
  }

  /* Setup the timer in the server_core epoll set */
  {
    command_handle->re_transmit_timer_private_data.endpoint_number = SL_CPC_ENDPOINT_SYSTEM; //Irrelevant in this scenario
    command_handle->re_transmit_timer_private_data.file_descriptor = timer_fd;
    command_handle->re_transmit_timer_private_data.callback = on_timer_expired;

    epoll_register(&command_handle->re_transmit_timer_private_data);
  }
}
