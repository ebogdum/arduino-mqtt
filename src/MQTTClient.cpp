#include "MQTTClient.h"

MQTT_ALWAYS_INLINE void lwmqtt_arduino_timer_set(void *ref, uint32_t timeout) {
  auto t = (lwmqtt_arduino_timer_t *)ref;
  t->timeout = timeout;
  t->start = (t->millis != nullptr) ? t->millis() : millis();
}

MQTT_ALWAYS_INLINE int32_t lwmqtt_arduino_timer_get(void *ref) {
  auto t = (lwmqtt_arduino_timer_t *)ref;
  
  // Get current time from custom source or Arduino millis
  uint32_t now = (t->millis != nullptr) ? t->millis() : millis();
  
  // Unsigned subtraction automatically handles rollover correctly
  uint32_t elapsed = now - t->start;
  
  // Return remaining time (negative if expired)
  return (int32_t)(t->timeout - elapsed);
}

MQTT_ALWAYS_INLINE lwmqtt_err_t lwmqtt_arduino_network_read(void *ref, uint8_t *buffer, size_t len, size_t *read,
                                                uint32_t timeout) {
  auto n = (lwmqtt_arduino_network_t *)ref;
  uint32_t start = millis();
  *read = 0;

  while (len > 0) {
    // Check timeout using unsigned subtraction (handles rollover)
    if (millis() - start >= timeout) {
      break;
    }

    int r = n->client->read(buffer, len);
    if (r > 0) {
      buffer += r;
      *read += r;
      len -= r;
    } else {
      // Yield to RTOS/WiFi task
      yield();
      
      // Check connection if no data is available
      if (!n->client->connected()) {
        return LWMQTT_NETWORK_FAILED_READ;
      }
    }
  }

  return (*read == 0) ? LWMQTT_NETWORK_TIMEOUT : LWMQTT_SUCCESS;
}

MQTT_ALWAYS_INLINE lwmqtt_err_t lwmqtt_arduino_network_write(void *ref, uint8_t *buffer, size_t len, size_t *sent,
                                                 uint32_t /*timeout*/) {
  auto n = (lwmqtt_arduino_network_t *)ref;
  *sent = n->client->write(buffer, len);
  return (*sent > 0) ? LWMQTT_SUCCESS : LWMQTT_NETWORK_FAILED_WRITE;
}

static void MQTTClientHandler(lwmqtt_client_t * /*client*/, void *ref, lwmqtt_string_t topic,
                              lwmqtt_message_t message) {
  auto cb = (MQTTClientCallback *)ref;
  
  // Quick exit if no callback set
  if (cb->type == MQTT_CB_NONE) return;

  // Zero-copy path: raw callbacks get untouched buffers and lengths (no mutation, no allocation)
  switch (cb->type) {
    case MQTT_CB_RAW:
      cb->raw(cb->client, topic.data, topic.len, (const char *)message.payload, message.payload_len);
      return;
#if MQTT_HAS_FUNCTIONAL
    case MQTT_CB_FUNC_RAW:
      cb->funcRaw(cb->client, topic.data, topic.len, (const char *)message.payload, message.payload_len);
      return;
#endif
    default:
      break;  // fall through to legacy paths that need C-strings/String
  }

  // Legacy paths below may require C-string termination; do so only when safe
  uint8_t *buf_base = cb->client ? cb->client->readBufferPtr() : nullptr;
  size_t buf_cap = cb->client ? cb->client->readBufferSize() + 1 : 0;  // +1 reserved by constructor
  uint8_t *buf_end = buf_base ? buf_base + buf_cap : nullptr;

  auto can_terminate = [&](const char *ptr, size_t len) -> bool {
    if (buf_base == nullptr) return false;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(ptr);
    return p >= buf_base && (p + len) < buf_end;  // strictly within reserved space
  };

  if (can_terminate(topic.data, topic.len)) {
    topic.data[topic.len] = '\0';
  }

  if (message.payload != nullptr && can_terminate(reinterpret_cast<char *>(message.payload), message.payload_len)) {
    message.payload[message.payload_len] = '\0';
  }

  // Dispatch based on callback type (union-based)
  switch (cb->type) {
    case MQTT_CB_ADVANCED:
      cb->advanced(cb->client, topic.data, (char *)message.payload, (int)message.payload_len);
      return;

#if MQTT_HAS_FUNCTIONAL
    case MQTT_CB_FUNC_ADVANCED:
      cb->funcAdvanced(cb->client, topic.data, (char *)message.payload, (int)message.payload_len);
      return;

    case MQTT_CB_FUNC_SIMPLE: {
      String str_topic(topic.data, topic.len);
      String str_payload((message.payload != nullptr) ? String((const char *)message.payload, message.payload_len) : String());
      cb->funcSimple(str_topic, str_payload);
      return;
    }
#endif

    case MQTT_CB_SIMPLE: {
      String str_topic(topic.data, topic.len);
      String str_payload((message.payload != nullptr) ? String((const char *)message.payload, message.payload_len) : String());
      cb->simple(str_topic, str_payload);
      return;
    }
    
    default:
      return;
  }
}

MQTTClient::MQTTClient(int readBufSize, int writeBufSize) {
  // Store buffer sizes
  this->readBufSize = (size_t)readBufSize;
  this->writeBufSize = (size_t)writeBufSize;
  
  // Allocate buffers (+1 for read buffer to allow null termination)
  this->readBuf = (uint8_t *)malloc(this->readBufSize + 1);
  this->writeBuf = (uint8_t *)malloc(this->writeBufSize);

   // Abort early if allocation failed; keep pointers null to prevent deref later
   if (this->readBuf == nullptr || this->writeBuf == nullptr) {
     free(this->readBuf);
     free(this->writeBuf);
     this->readBuf = nullptr;
     this->writeBuf = nullptr;
     this->readBufSize = 0;
     this->writeBufSize = 0;
   }
  
  // Initialize callback to none
  this->callback.client = nullptr;
  this->callback.type = MQTT_CB_NONE;
  this->callback.simple = nullptr;
}

MQTTClient::~MQTTClient() {
  this->destroyCallback();
  // free will
  this->clearWill();

  // free hostname
  if (this->hostname != nullptr) {
    free((void *)this->hostname);
  }

  // free buffers
  free(this->readBuf);
  free(this->writeBuf);
}

void MQTTClient::begin(Client &_client) {
  // Abort if buffers were not allocated
  if (this->readBuf == nullptr || this->writeBuf == nullptr) {
    this->_lastError = LWMQTT_BUFFER_TOO_SHORT;
    return;
  }
  // set client
  this->netClient = &_client;

  // initialize client
  lwmqtt_init(&this->client, this->writeBuf, this->writeBufSize, this->readBuf, this->readBufSize);

  // set timers
  lwmqtt_set_timers(&this->client, &this->timer1, &this->timer2, lwmqtt_arduino_timer_set, lwmqtt_arduino_timer_get);

  // set network
  lwmqtt_set_network(&this->client, &this->network, lwmqtt_arduino_network_read, lwmqtt_arduino_network_write);

  // set callback
  lwmqtt_set_callback(&this->client, (void *)&this->callback, MQTTClientHandler);
}

void MQTTClient::onMessage(MQTTClientCallbackSimple cb) {
  this->destroyCallback();
  this->callback.client = this;
  this->callback.type = MQTT_CB_SIMPLE;
  this->callback.simple = cb;
}

void MQTTClient::onMessageAdvanced(MQTTClientCallbackAdvanced cb) {
  this->destroyCallback();
  this->callback.client = this;
  this->callback.type = MQTT_CB_ADVANCED;
  this->callback.advanced = cb;
}

void MQTTClient::onMessageRaw(MQTTClientCallbackRaw cb) {
  this->destroyCallback();
  this->callback.client = this;
  this->callback.type = MQTT_CB_RAW;
  this->callback.raw = cb;
}

#if MQTT_HAS_FUNCTIONAL
void MQTTClient::onMessage(MQTTClientCallbackSimpleFunction cb) {
  this->destroyCallback();
  this->callback.client = this;
  this->callback.type = MQTT_CB_FUNC_SIMPLE;
  new (&this->callback.funcSimple) MQTTClientCallbackSimpleFunction(cb);
}

void MQTTClient::onMessageAdvanced(MQTTClientCallbackAdvancedFunction cb) {
  this->destroyCallback();
  this->callback.client = this;
  this->callback.type = MQTT_CB_FUNC_ADVANCED;
  new (&this->callback.funcAdvanced) MQTTClientCallbackAdvancedFunction(cb);
}

void MQTTClient::onMessageRaw(MQTTClientCallbackRawFunction cb) {
  this->destroyCallback();
  this->callback.client = this;
  this->callback.type = MQTT_CB_FUNC_RAW;
  new (&this->callback.funcRaw) MQTTClientCallbackRawFunction(cb);
}
#endif

void MQTTClient::setClockSource(MQTTClientClockSource cb) {
  this->timer1.millis = cb;
  this->timer2.millis = cb;
}

void MQTTClient::setHost(IPAddress _address, int _port) {
  // set address and port
  this->address = _address;
  this->port = _port;
}

void MQTTClient::setHost(const char _hostname[], int _port) {
  // free hostname if set
  if (this->hostname != nullptr) {
    free((void *)this->hostname);
  }

  // set hostname and port
  this->hostname = strdup(_hostname);
  this->port = _port;
}

void MQTTClient::setWill(const char topic[], const char payload[], bool retained, int qos) {
  // Quick validation
  if (topic == nullptr || *topic == '\0') {
    return;
  }

  // Clear any existing will
  this->clearWill();

  // Allocate and zero-initialize will structure
  this->will = (lwmqtt_will_t *)malloc(sizeof(lwmqtt_will_t));
  if (this->will == nullptr) return;
  memset(this->will, 0, sizeof(lwmqtt_will_t));

  // Set topic (strdup handles strlen internally)
  char *topic_copy = strdup(topic);
  if (topic_copy == nullptr) {
    this->clearWill();
    return;
  }
  this->will->topic = lwmqtt_string(topic_copy);

  // Set payload if provided
  if (payload != nullptr && *payload != '\0') {
    char *payload_copy = strdup(payload);
    if (payload_copy == nullptr) {
      this->clearWill();
      return;
    }
    this->will->payload = lwmqtt_string(payload_copy);
  }

  // Set flags
  this->will->retained = retained;
  this->will->qos = (lwmqtt_qos_t)qos;
}

void MQTTClient::clearWill() {
  // return if not set
  if (this->will == nullptr) {
    return;
  }

  // free payload if set
  if (this->will->payload.len > 0) {
    free(this->will->payload.data);
  }

  // free topic if set
  if (this->will->topic.len > 0) {
    free(this->will->topic.data);
  }

  // free will
  free(this->will);
  this->will = nullptr;
}

void MQTTClient::setKeepAlive(int _keepAlive) { this->keepAlive = _keepAlive; }

void MQTTClient::setCleanSession(bool _cleanSession) { this->cleanSession = _cleanSession; }

void MQTTClient::setTimeout(int _timeout) { this->timeout = _timeout; }

void MQTTClient::dropOverflow(bool enabled) {
  // configure drop overflow
  lwmqtt_drop_overflow(&this->client, enabled, &this->_droppedMessages);
}

bool MQTTClient::connect(const char clientID[], const char username[], const char password[], bool skip) {
  // close left open connection if still connected
  if (!skip && this->connected()) {
    this->close();
  }

  // save client
  this->network.client = this->netClient;

  // connect to host
  if (!skip) {
    int ret;
    if (this->hostname != nullptr) {
      ret = this->netClient->connect(this->hostname, (uint16_t)this->port);
    } else {
      ret = this->netClient->connect(this->address, (uint16_t)this->port);
    }
    if (ret <= 0) {
      this->_lastError = LWMQTT_NETWORK_FAILED_CONNECT;
      return false;
    }
  }

  // prepare options
  lwmqtt_connect_options_t options = lwmqtt_default_connect_options;
  options.keep_alive = this->keepAlive;
  options.clean_session = this->cleanSession;
  options.client_id = lwmqtt_string(clientID);

  // set username and password if available
  if (username != nullptr) {
    options.username = lwmqtt_string(username);
  }
  if (password != nullptr) {
    options.password = lwmqtt_string(password);
  }

  // connect to broker
  this->_lastError = lwmqtt_connect(&this->client, &options, this->will, this->timeout);

  // copy return code
  this->_returnCode = options.return_code;

  // handle error
  if (this->_lastError != LWMQTT_SUCCESS) {
    // close connection
    this->close();

    return false;
  }

  // copy session present flag
  this->_sessionPresent = options.session_present;

  // set flag
  this->_connected = true;

  return true;
}

bool MQTTClient::publish(const char topic[], const char payload[], int length, bool retained, int qos) {
  // return immediately if not connected
  if (!this->connected()) {
    return false;
  }

  // prepare message
  lwmqtt_message_t message = lwmqtt_default_message;
  message.payload = (uint8_t *)payload;
  message.payload_len = (size_t)length;
  message.retained = retained;
  message.qos = lwmqtt_qos_t(qos);

  // prepare options
  lwmqtt_publish_options_t options = lwmqtt_default_publish_options;

  // set duplicate packet id if available
  if (this->nextDupPacketID > 0) {
    options.dup_id = &this->nextDupPacketID;
    this->nextDupPacketID = 0;
  }

  // publish message
  this->_lastError = lwmqtt_publish(&this->client, &options, lwmqtt_string(topic), message, this->timeout);
  if (this->_lastError != LWMQTT_SUCCESS) {
    // close connection
    this->close();

    return false;
  }

  return true;
}

uint16_t MQTTClient::lastPacketID() {
  // get last packet id from client
  return this->client.last_packet_id;
}

void MQTTClient::prepareDuplicate(uint16_t packetID) {
  // set next duplicate packet id
  this->nextDupPacketID = packetID;
}

bool MQTTClient::subscribe(const char topic[], int qos) {
  // return immediately if not connected
  if (!this->connected()) {
    return false;
  }

  // subscribe to topic
  this->_lastError = lwmqtt_subscribe_one(&this->client, lwmqtt_string(topic), (lwmqtt_qos_t)qos, this->timeout);
  if (this->_lastError != LWMQTT_SUCCESS) {
    // close connection
    this->close();

    return false;
  }

  return true;
}

bool MQTTClient::unsubscribe(const char topic[]) {
  // return immediately if not connected
  if (!this->connected()) {
    return false;
  }

  // unsubscribe from topic
  this->_lastError = lwmqtt_unsubscribe_one(&this->client, lwmqtt_string(topic), this->timeout);
  if (this->_lastError != LWMQTT_SUCCESS) {
    // close connection
    this->close();

    return false;
  }

  return true;
}

bool MQTTClient::loop() {
  // return immediately if not connected
  if (!this->connected()) {
    return false;
  }

  // get available bytes on the network
  int available = this->netClient->available();

  // yield if data is available
  if (available > 0) {
    this->_lastError = lwmqtt_yield(&this->client, available, this->timeout);
    if (this->_lastError != LWMQTT_SUCCESS) {
      // close connection
      this->close();

      return false;
    }
  }

  // keep the connection alive
  this->_lastError = lwmqtt_keep_alive(&this->client, this->timeout);
  if (this->_lastError != LWMQTT_SUCCESS) {
    // close connection
    this->close();

    return false;
  }

  return true;
}

bool MQTTClient::connected() {
  // Check internal flag first (cheapest), then validate network state
  return this->_connected && this->netClient != nullptr && this->netClient->connected() == 1;
}

bool MQTTClient::disconnect() {
  // return immediately if not connected anymore
  if (!this->connected()) {
    return false;
  }

  // cleanly disconnect
  this->_lastError = lwmqtt_disconnect(&this->client, this->timeout);

  // close
  this->close();

  return this->_lastError == LWMQTT_SUCCESS;
}

void MQTTClient::close() {
  // set flag
  this->_connected = false;

  // close network
  this->netClient->stop();
}

void MQTTClient::destroyCallback() {
  this->callback.clear();
}
