#include <string.h>

#include <lwmqtt.h>

#include "packet.h"

void lwmqtt_init(lwmqtt_client_t *client, unsigned char *write_buf, int write_buf_size, unsigned char *read_buf,
                 int read_buf_size) {
  client->next_packet_id = 1;
  client->keep_alive_interval = 0;
  client->ping_outstanding = false;

  client->write_buf = write_buf;
  client->write_buf_size = write_buf_size;
  client->read_buf = read_buf;
  client->read_buf_size = read_buf_size;
  client->callback = NULL;

  client->network = NULL;
  client->network_peek = NULL;
  client->network_read = NULL;
  client->network_write = NULL;

  client->keep_alive_timer = NULL;
  client->command_timer = NULL;
  client->timer_set = NULL;
  client->timer_get = NULL;
}

void lwmqtt_set_network(lwmqtt_client_t *client, void *ref, lwmqtt_network_peek_t peek, lwmqtt_network_read_t read,
                        lwmqtt_network_write_t write) {
  client->network = ref;
  client->network_peek = peek;
  client->network_read = read;
  client->network_write = write;
}

void lwmqtt_set_timers(lwmqtt_client_t *client, void *keep_alive_timer, void *network_timer, lwmqtt_timer_set_t set,
                       lwmqtt_timer_get_t get) {
  client->keep_alive_timer = keep_alive_timer;
  client->command_timer = network_timer;
  client->timer_set = set;
  client->timer_get = get;

  client->timer_set(client, client->keep_alive_timer, 0);
  client->timer_set(client, client->command_timer, 0);
}

void lwmqtt_set_callback(lwmqtt_client_t *client, lwmqtt_callback_t cb) { client->callback = cb; }

static unsigned short lwmqtt_get_next_packet_id(lwmqtt_client_t *c) {
  return c->next_packet_id = (unsigned short)((c->next_packet_id == 65535) ? 1 : c->next_packet_id + 1);
}

static lwmqtt_err_t lwmqtt_read_packet(lwmqtt_client_t *c, lwmqtt_packet_type_t *packet_type) {
  // peek available bytes if supported
  if (c->network_peek != NULL) {
    // get available bytes
    int available;
    lwmqtt_err_t err = c->network_peek(c, c->network, &available);
    if (err != LWMQTT_SUCCESS) {
      return err;
    }

    // return if no bytes are available
    if (available == 0) {
      *packet_type = LWMQTT_NO_PACKET;
      return LWMQTT_SUCCESS;
    }
  }

  // read header byte
  int read = 0;
  lwmqtt_err_t err = c->network_read(c, c->network, c->read_buf, 1, &read, c->timer_get(c, c->command_timer));
  if (err != LWMQTT_SUCCESS) {
    return err;
  } else if (read == 0) {
    *packet_type = LWMQTT_NO_PACKET;
    return LWMQTT_SUCCESS;
  }

  // detect packet type
  *packet_type = lwmqtt_detect_packet_type(c->read_buf);
  if (*packet_type == LWMQTT_INVALID_PACKET) {
    return LWMQTT_FAILURE;
  }

  // prepare variables
  int len = 0;
  int rem_len = 0;

  do {
    // adjust len
    len++;

    // read next byte
    read = 0;
    err = c->network_read(c, c->network, c->read_buf + len, 1, &read, c->timer_get(c, c->command_timer));
    if (err != LWMQTT_SUCCESS) {
      return err;
    } else if (read != 1) {
      return LWMQTT_NOT_ENOUGH_DATA;
    }

    // attempt to detect remaining length
    err = lwmqtt_detect_remaining_length(c->read_buf + 1, len, &rem_len);
  } while (err == LWMQTT_BUFFER_TOO_SHORT);

  // check final error
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // read the rest of the buffer if needed
  if (rem_len > 0) {
    read = 0;
    err = c->network_read(c, c->network, c->read_buf + 1 + len, rem_len, &read, c->timer_get(c, c->command_timer));
    if (err != LWMQTT_SUCCESS) {
      return err;
    } else if (read != rem_len) {
      return LWMQTT_NOT_ENOUGH_DATA;
    }
  }

  return LWMQTT_SUCCESS;
}

static lwmqtt_err_t lwmqtt_send_packet(lwmqtt_client_t *c, int length) {
  // write to network
  int sent = 0;
  lwmqtt_err_t err = c->network_write(c, c->network, c->write_buf, length, &sent, c->timer_get(c, c->command_timer));
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // check length
  if (sent != length) {
    return LWMQTT_NOT_ENOUGH_DATA;
  }

  // reset keep alive timer
  c->timer_set(c, c->keep_alive_timer, c->keep_alive_interval * 1000);

  return LWMQTT_SUCCESS;
}

static lwmqtt_err_t lwmqtt_cycle(lwmqtt_client_t *c, lwmqtt_packet_type_t *packet_type) {
  // read next packet from the network
  lwmqtt_err_t err = lwmqtt_read_packet(c, packet_type);
  if (err != LWMQTT_SUCCESS) {
    return err;
  } else if (*packet_type == LWMQTT_NO_PACKET) {
    return LWMQTT_SUCCESS;
  }

  switch (*packet_type) {
    // handle publish packets
    case LWMQTT_PUBLISH_PACKET: {
      // decode publish packet
      lwmqtt_string_t topic = lwmqtt_default_string;
      lwmqtt_message_t msg;
      bool dup;
      unsigned short packet_id;
      err = lwmqtt_decode_publish(&dup, &msg.qos, &msg.retained, &packet_id, &topic, (unsigned char **)&msg.payload,
                                  &msg.payload_len, c->read_buf, c->read_buf_size);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      // call callback if set
      if (c->callback != NULL) {
        c->callback(c, &topic, &msg);
      }

      // break early of qos zero
      if (msg.qos == LWMQTT_QOS0) {
        break;
      }

      // define ack packet
      lwmqtt_packet_type_t ack_type = LWMQTT_NO_PACKET;
      if (msg.qos == LWMQTT_QOS1) {
        ack_type = LWMQTT_PUBREC_PACKET;
      } else if (msg.qos == LWMQTT_QOS2) {
        ack_type = LWMQTT_PUBREL_PACKET;
      }

      // encode ack packet
      int len;
      err = lwmqtt_encode_ack(c->write_buf, c->write_buf_size, &len, ack_type, false, packet_id);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      // send ack packet
      err = lwmqtt_send_packet(c, len);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      break;
    }

    // handle pubrec packets
    case LWMQTT_PUBREC_PACKET: {
      // decode pubrec packet
      bool dup;
      unsigned short packet_id;
      err = lwmqtt_decode_ack(packet_type, &dup, &packet_id, c->read_buf, c->read_buf_size);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      // encode pubrel packet
      int len;
      err = lwmqtt_encode_ack(c->write_buf, c->write_buf_size, &len, LWMQTT_PUBREL_PACKET, 0, packet_id);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      // send pubrel packet
      err = lwmqtt_send_packet(c, len);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      break;
    }

    // handle pubrel packets
    case LWMQTT_PUBREL_PACKET: {
      // decode pubrec packet
      bool dup;
      unsigned short packet_id;
      err = lwmqtt_decode_ack(packet_type, &dup, &packet_id, c->read_buf, c->read_buf_size);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      // encode pubcomp packet
      int len;
      err = lwmqtt_encode_ack(c->write_buf, c->write_buf_size, &len, LWMQTT_PUBCOMP_PACKET, 0, packet_id);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      // send pubcomp packet
      err = lwmqtt_send_packet(c, len);
      if (err != LWMQTT_SUCCESS) {
        return err;
      }

      break;
    }

    // handle pingresp packets
    case LWMQTT_PINGRESP_PACKET: {
      // set flag
      c->ping_outstanding = false;

      break;
    }

    // handle all other packets
    default: { break; }
  }

  return LWMQTT_SUCCESS;
}

static lwmqtt_err_t lwmqtt_cycle_until(lwmqtt_client_t *c, lwmqtt_packet_type_t *packet_type,
                                       lwmqtt_packet_type_t needle) {
  // loop until timeout has been reached
  do {
    // do one cycle
    lwmqtt_err_t err = lwmqtt_cycle(c, packet_type);
    if (err != LWMQTT_SUCCESS) {
      return err;
    }

    // check if needle has been found
    if (needle != LWMQTT_NO_PACKET && *packet_type == needle) {
      return LWMQTT_SUCCESS;
    }
  } while (c->timer_get(c, c->command_timer) > 0);

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_yield(lwmqtt_client_t *client, unsigned int timeout) {
  // set timeout
  client->timer_set(client, client->command_timer, timeout);

  // cycle until timeout has been reached
  lwmqtt_packet_type_t packet_type = LWMQTT_NO_PACKET;
  lwmqtt_err_t err = lwmqtt_cycle_until(client, &packet_type, LWMQTT_NO_PACKET);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_connect(lwmqtt_client_t *client, lwmqtt_options_t *options, lwmqtt_will_t *will,
                            lwmqtt_return_code_t *return_code, unsigned int timeout) {
  // set timer to command timeout
  client->timer_set(client, client->command_timer, timeout);

  // save keep alive interval
  client->keep_alive_interval = options->keep_alive;

  // set keep alive timer
  if (client->keep_alive_interval > 0) {
    client->timer_set(client, client->keep_alive_timer, client->keep_alive_interval * 1000);
  }

  // encode connect packet
  int len;
  if (lwmqtt_encode_connect(client->write_buf, client->write_buf_size, &len, options, will) != LWMQTT_SUCCESS) {
    return LWMQTT_FAILURE;
  }

  // send packet
  lwmqtt_err_t err = lwmqtt_send_packet(client, len);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // wait for connack packet
  lwmqtt_packet_type_t packet_type = LWMQTT_NO_PACKET;
  err = lwmqtt_cycle_until(client, &packet_type, LWMQTT_CONNACK_PACKET);
  if (err != LWMQTT_SUCCESS) {
    return err;
  } else if (packet_type != LWMQTT_CONNACK_PACKET) {
    return LWMQTT_FAILURE;
  }

  // decode connack packet
  bool session_present;
  if (lwmqtt_decode_connack(&session_present, return_code, client->read_buf, client->read_buf_size) != LWMQTT_SUCCESS) {
    return LWMQTT_FAILURE;
  }

  // return error if connection was not accepted
  if (*return_code != LWMQTT_CONNACK_CONNECTION_ACCEPTED) {
    return LWMQTT_FAILURE;
  }

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_subscribe(lwmqtt_client_t *client, const char *topic_filter, lwmqtt_qos_t qos,
                              unsigned int timeout) {
  // set timeout
  client->timer_set(client, client->command_timer, timeout);

  // prepare string
  lwmqtt_string_t str = lwmqtt_default_string;
  str.c_string = (char *)topic_filter;

  // encode subscribe packet
  int len;
  lwmqtt_err_t err = lwmqtt_encode_subscribe(client->write_buf, client->write_buf_size, &len,
                                             lwmqtt_get_next_packet_id(client), 1, &str, &qos);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // send packet
  err = lwmqtt_send_packet(client, len);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // wait for suback packet
  lwmqtt_packet_type_t packet_type = LWMQTT_NO_PACKET;
  err = lwmqtt_cycle_until(client, &packet_type, LWMQTT_SUBACK_PACKET);
  if (err != LWMQTT_SUCCESS) {
    return err;
  } else if (packet_type != LWMQTT_SUBACK_PACKET) {
    return LWMQTT_FAILURE;
  }

  // decode packet
  int count = 0;
  lwmqtt_qos_t grantedQoS;
  unsigned short packet_id;
  err = lwmqtt_decode_suback(&packet_id, 1, &count, &grantedQoS, client->read_buf, client->read_buf_size);
  if (err == LWMQTT_SUCCESS) {
    return err;
  }

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_unsubscribe(lwmqtt_client_t *client, const char *topic_filter, unsigned int timeout) {
  // set timer
  client->timer_set(client, client->command_timer, timeout);

  // prepare string
  lwmqtt_string_t str = lwmqtt_default_string;
  str.c_string = (char *)topic_filter;

  // encode unsubscribe packet
  int len;
  lwmqtt_err_t err = lwmqtt_encode_unsubscribe(client->write_buf, client->write_buf_size, &len,
                                               lwmqtt_get_next_packet_id(client), 1, &str);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // send unsubscribe packet
  err = lwmqtt_send_packet(client, len);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // wait for unsuback packet
  lwmqtt_packet_type_t packet_type = LWMQTT_NO_PACKET;
  err = lwmqtt_cycle_until(client, &packet_type, LWMQTT_UNSUBACK_PACKET);
  if (err != LWMQTT_SUCCESS) {
    return err;
  } else if (packet_type != LWMQTT_UNSUBACK_PACKET) {
    return LWMQTT_FAILURE;
  }

  // decode unsuback packet
  bool dup;
  unsigned short packet_id;
  err = lwmqtt_decode_ack(&packet_type, &dup, &packet_id, client->read_buf, client->read_buf_size);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_publish(lwmqtt_client_t *client, const char *topicName, lwmqtt_message_t *message,
                            unsigned int timeout) {
  // prepare string
  lwmqtt_string_t str = lwmqtt_default_string;
  str.c_string = (char *)topicName;

  // set timer
  client->timer_set(client, client->command_timer, timeout);

  // add packet id if at least qos 1
  unsigned short packet_id = 0;
  if (message->qos == LWMQTT_QOS1 || message->qos == LWMQTT_QOS2) {
    packet_id = lwmqtt_get_next_packet_id(client);
  }

  // encode publish packet
  int len = 0;
  lwmqtt_err_t err = lwmqtt_encode_publish(client->write_buf, client->write_buf_size, &len, 0, message->qos,
                                           (char)(message->retained ? 1 : 0), packet_id, str,
                                           (unsigned char *)message->payload, message->payload_len);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // send packet
  err = lwmqtt_send_packet(client, len);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // immediately return on qos zero
  if (message->qos == LWMQTT_QOS0) {
    return LWMQTT_SUCCESS;
  }

  // define ack packet
  lwmqtt_packet_type_t ack_type = LWMQTT_NO_PACKET;
  if (message->qos == LWMQTT_QOS1) {
    ack_type = LWMQTT_PUBACK_PACKET;
  } else if (message->qos == LWMQTT_QOS2) {
    ack_type = LWMQTT_PUBCOMP_PACKET;
  }

  // wait for ack packet
  lwmqtt_packet_type_t packet_type = LWMQTT_NO_PACKET;
  err = lwmqtt_cycle_until(client, &packet_type, ack_type);
  if (err != LWMQTT_SUCCESS) {
    return err;
  } else if (packet_type != ack_type) {
    return LWMQTT_FAILURE;
  }

  // decode ack packet
  bool dup;
  err = lwmqtt_decode_ack(&packet_type, &dup, &packet_id, client->read_buf, client->read_buf_size);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_disconnect(lwmqtt_client_t *client, unsigned int timeout) {
  // set timer
  client->timer_set(client, client->command_timer, timeout);

  // encode disconnect packet
  int len;
  if (lwmqtt_encode_zero(client->write_buf, client->write_buf_size, &len, LWMQTT_DISCONNECT_PACKET) != LWMQTT_SUCCESS) {
    return LWMQTT_FAILURE;
  }

  // send disconnected packet
  lwmqtt_err_t err = lwmqtt_send_packet(client, len);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_keep_alive(lwmqtt_client_t *client, unsigned int timeout) {
  // set timer
  client->timer_set(client, client->command_timer, timeout);

  // return immediately if keep alive interval is zero
  if (client->keep_alive_interval == 0) {
    return LWMQTT_SUCCESS;
  }

  // return immediately if no ping is due
  if (client->timer_get(client, client->keep_alive_timer) > 0) {
    return LWMQTT_SUCCESS;
  }

  // a ping is due

  // fail immediately if a ping is still outstanding
  if (client->ping_outstanding) {
    return LWMQTT_FAILURE;
  }

  // encode pingreq packet
  int len;
  lwmqtt_err_t err = lwmqtt_encode_zero(client->write_buf, client->write_buf_size, &len, LWMQTT_PINGREQ_PACKET);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // send packet
  err = lwmqtt_send_packet(client, len);
  if (err != LWMQTT_SUCCESS) {
    return err;
  }

  // set flag
  client->ping_outstanding = true;

  return LWMQTT_SUCCESS;
}
