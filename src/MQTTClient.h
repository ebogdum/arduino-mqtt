#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

// Allow users to disable std::function support to save ~64 bytes RAM per client
// Define MQTT_NO_FUNCTIONAL before including this header to disable
#ifndef MQTT_NO_FUNCTIONAL
  #if defined(ESP8266) || (defined ESP32)
  #include <functional>
  #define MQTT_HAS_FUNCTIONAL 1
  #elif defined(__has_include)
  #if __has_include(<functional>)
  #if defined(min)
  #undef min
  #endif
  #if defined(max)
  #undef max
  #endif
  #include <functional>
  #define MQTT_HAS_FUNCTIONAL 1
  #else
  #define MQTT_HAS_FUNCTIONAL 0
  #endif
  #else
  #define MQTT_HAS_FUNCTIONAL 0
  #endif
#else
  #define MQTT_HAS_FUNCTIONAL 0
#endif

#include <Arduino.h>
#include <Client.h>
#include <Stream.h>

extern "C" {
#include "lwmqtt/lwmqtt.h"
}

// Force inline for performance-critical functions
#if defined(__GNUC__) || defined(__clang__)
  #define MQTT_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
  #define MQTT_ALWAYS_INLINE inline
#endif

typedef uint32_t (*MQTTClientClockSource)();

typedef struct {
  uint32_t start;
  uint32_t timeout;
  MQTTClientClockSource millis;
} lwmqtt_arduino_timer_t;

typedef struct {
  Client *client;
} lwmqtt_arduino_network_t;

class MQTTClient;

typedef void (*MQTTClientCallbackSimple)(String &topic, String &payload);
typedef void (*MQTTClientCallbackAdvanced)(MQTTClient *client, char topic[], char bytes[], int length);
typedef void (*MQTTClientCallbackRaw)(MQTTClient *client, const char *topic, size_t topic_len, const char *payload,
                    size_t payload_len);
#if MQTT_HAS_FUNCTIONAL
typedef std::function<void(String &topic, String &payload)> MQTTClientCallbackSimpleFunction;
typedef std::function<void(MQTTClient *client, char topic[], char bytes[], int length)>
    MQTTClientCallbackAdvancedFunction;
typedef std::function<void(MQTTClient *client, const char *topic, size_t topic_len, const char *payload,
               size_t payload_len)>
  MQTTClientCallbackRawFunction;
#endif

// Callback type enumeration for union-based storage
enum MQTTCallbackType : uint8_t {
  MQTT_CB_NONE = 0,
  MQTT_CB_SIMPLE = 1,
  MQTT_CB_ADVANCED = 2,
  MQTT_CB_RAW = 3,
#if MQTT_HAS_FUNCTIONAL
  MQTT_CB_FUNC_SIMPLE = 4,
  MQTT_CB_FUNC_ADVANCED = 5,
  MQTT_CB_FUNC_RAW = 6
#endif
};

// Optimized callback structure using union - saves 48+ bytes vs storing all pointers
struct MQTTClientCallback {
  MQTTClient *client;
  MQTTCallbackType type;
  union {
    MQTTClientCallbackSimple simple;
    MQTTClientCallbackAdvanced advanced;
    MQTTClientCallbackRaw raw;
#if MQTT_HAS_FUNCTIONAL
    MQTTClientCallbackSimpleFunction funcSimple;
    MQTTClientCallbackAdvancedFunction funcAdvanced;
    MQTTClientCallbackRawFunction funcRaw;
#endif
  };
  
  MQTTClientCallback() : client(nullptr), type(MQTT_CB_NONE), simple(nullptr) {}

  ~MQTTClientCallback() {
#if MQTT_HAS_FUNCTIONAL
    switch (type) {
      case MQTT_CB_FUNC_SIMPLE:
        funcSimple.~MQTTClientCallbackSimpleFunction();
        break;
      case MQTT_CB_FUNC_ADVANCED:
        funcAdvanced.~MQTTClientCallbackAdvancedFunction();
        break;
      case MQTT_CB_FUNC_RAW:
        funcRaw.~MQTTClientCallbackRawFunction();
        break;
      default:
        break;
    }
#endif
  }

  void clear() {
    this->~MQTTClientCallback();
    type = MQTT_CB_NONE;
    simple = nullptr;
  }
};

class MQTTClient {
 private:
  // Pointers (8 bytes on 64-bit, 4 on 32-bit)
  uint8_t *readBuf = nullptr;
  uint8_t *writeBuf = nullptr;
  Client *netClient = nullptr;
  const char *hostname = nullptr;
  lwmqtt_will_t *will = nullptr;

  // Structs (contain pointers and data)
  MQTTClientCallback callback;
  lwmqtt_arduino_network_t network = {nullptr};
  lwmqtt_arduino_timer_t timer1 = {0, 0, nullptr};
  lwmqtt_arduino_timer_t timer2 = {0, 0, nullptr};
  lwmqtt_client_t client = lwmqtt_client_t();
  IPAddress address;

  // 4-byte aligned data
  size_t readBufSize = 0;
  size_t writeBufSize = 0;
  uint32_t timeout = 1000;
  uint32_t _droppedMessages = 0;
  int port = 0;

  // 2-byte aligned data
  uint16_t keepAlive = 10;
  uint16_t nextDupPacketID = 0;

  // 1-byte aligned data
  bool cleanSession = true;
  bool _sessionPresent = false;
  bool _connected = false;
  
  // Enums (usually int, but can be smaller)
  lwmqtt_return_code_t _returnCode = (lwmqtt_return_code_t)0;
  lwmqtt_err_t _lastError = (lwmqtt_err_t)0;

 public:
  void *ref = nullptr;

  // Default buffer size reduced to 64 bytes (was 128) - sufficient for most topics/payloads
  // Use larger buffers explicitly if needed: MQTTClient mqtt(256);
  explicit MQTTClient(int bufSize = 64) : MQTTClient(bufSize, bufSize) {}
  MQTTClient(int readBufSize, int writeBufSize);

  ~MQTTClient();

  void begin(Client &_client);
  void begin(const char _hostname[], Client &_client) { this->begin(_hostname, 1883, _client); }
  void begin(const char _hostname[], int _port, Client &_client) {
    this->begin(_client);
    this->setHost(_hostname, _port);
  }
  void begin(IPAddress _address, Client &_client) { this->begin(_address, 1883, _client); }
  void begin(IPAddress _address, int _port, Client &_client) {
    this->begin(_client);
    this->setHost(_address, _port);
  }

  void onMessage(MQTTClientCallbackSimple cb);
  void onMessageAdvanced(MQTTClientCallbackAdvanced cb);
  void onMessageRaw(MQTTClientCallbackRaw cb);
#if MQTT_HAS_FUNCTIONAL
  void onMessage(MQTTClientCallbackSimpleFunction cb);
  void onMessageAdvanced(MQTTClientCallbackAdvancedFunction cb);
  void onMessageRaw(MQTTClientCallbackRawFunction cb);
#endif

  void setClockSource(MQTTClientClockSource cb);

  void setHost(const char _hostname[]) { this->setHost(_hostname, 1883); }
  void setHost(const char hostname[], int port);
  void setHost(IPAddress _address) { this->setHost(_address, 1883); }
  void setHost(IPAddress _address, int port);

  void setWill(const char topic[]) { this->setWill(topic, ""); }
  void setWill(const char topic[], const char payload[]) { this->setWill(topic, payload, false, 0); }
  void setWill(const char topic[], const char payload[], bool retained, int qos);
  void clearWill();

  void setKeepAlive(int keepAlive);
  void setCleanSession(bool cleanSession);
  void setTimeout(int timeout);
  void setOptions(int _keepAlive, bool _cleanSession, int _timeout) {
    this->setKeepAlive(_keepAlive);
    this->setCleanSession(_cleanSession);
    this->setTimeout(_timeout);
  }

  void dropOverflow(bool enabled);
  uint32_t droppedMessages() { return this->_droppedMessages; }

  bool connect(const char clientId[], bool skip = false) { return this->connect(clientId, nullptr, nullptr, skip); }
  bool connect(const char clientId[], const char username[], bool skip = false) {
    return this->connect(clientId, username, nullptr, skip);
  }
  bool connect(const char clientID[], const char username[], const char password[], bool skip = false);

  bool publish(const String &topic) { return this->publish(topic.c_str(), ""); }
  bool publish(const char topic[]) { return this->publish(topic, ""); }
  bool publish(const String &topic, const String &payload) { return this->publish(topic.c_str(), payload.c_str()); }
  bool publish(const String &topic, const String &payload, bool retained, int qos) {
    return this->publish(topic.c_str(), payload.c_str(), retained, qos);
  }
  bool publish(const char topic[], const String &payload) { return this->publish(topic, payload.c_str()); }
  bool publish(const char topic[], const String &payload, bool retained, int qos) {
    return this->publish(topic, payload.c_str(), retained, qos);
  }
  bool publish(const char topic[], const char payload[]) {
    return this->publish(topic, (char *)payload, (int)strlen(payload));
  }
  bool publish(const char topic[], const char payload[], bool retained, int qos) {
    return this->publish(topic, (char *)payload, (int)strlen(payload), retained, qos);
  }
  bool publish(const char topic[], const char payload[], int length) {
    return this->publish(topic, payload, length, false, 0);
  }
  bool publish(const char topic[], const char payload[], int length, bool retained, int qos);

  uint16_t lastPacketID();
  void prepareDuplicate(uint16_t packetID);

  bool subscribe(const String &topic) { return this->subscribe(topic.c_str()); }
  bool subscribe(const String &topic, int qos) { return this->subscribe(topic.c_str(), qos); }
  bool subscribe(const char topic[]) { return this->subscribe(topic, 0); }
  bool subscribe(const char topic[], int qos);

  bool unsubscribe(const String &topic) { return this->unsubscribe(topic.c_str()); }
  bool unsubscribe(const char topic[]);

  bool loop();
  bool connected();
  bool sessionPresent() { return this->_sessionPresent; }

    // Expose buffer info for internal handlers (kept small to avoid copying)
    uint8_t *readBufferPtr() { return this->readBuf; }
    size_t readBufferSize() const { return this->readBufSize; }

  lwmqtt_err_t lastError() { return this->_lastError; }
  lwmqtt_return_code_t returnCode() { return this->_returnCode; }

  bool disconnect();

 private:
    void destroyCallback();
  void close();
};

#endif
