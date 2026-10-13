#include "NodeRadio.h"
#include "ExerciseConfig.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

namespace {
  QueueHandle_t incoming = nullptr;
  QueueHandle_t outgoing = nullptr;
  QueueHandle_t completion = nullptr;
  bool started = false;
  bool sending = false;
  NodeRole role = HIP;
  const uint8_t* peerMac() {
    return role == HIP ? NodeConfig::WRIST_MAC : NodeConfig::HIP_MAC;
  }

  void receivePacket(const uint8_t* mac, const uint8_t* bytes, int size) {
    if (!incoming || size != (int)sizeof(LinkPacket) || memcmp(mac, peerMac(), 6)) return;
    LinkPacket packet;
    memcpy(&packet, bytes, sizeof(packet));
    xQueueSend(incoming, &packet, 0);
  }

#if ESP_IDF_VERSION_MAJOR >= 5
  void onReceive(const esp_now_recv_info_t* info, const uint8_t* bytes, int size) {
    receivePacket(info->src_addr, bytes, size);
  }
#else
  void onReceive(const uint8_t* mac, const uint8_t* bytes, int size) {
    receivePacket(mac, bytes, size);
  }
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  void onSent(const esp_now_send_info_t*, esp_now_send_status_t) {
#else
  void onSent(const uint8_t*, esp_now_send_status_t) {
#endif
    const uint8_t done = 1;
    xQueueOverwrite(completion, &done);
  }
}

bool NodeRadio::begin(NodeRole nodeRole) {
  role = nodeRole;
  if (!WiFi.mode(WIFI_STA)) return false;
  Serial.printf("[Role: %s | Wi-Fi STA MAC: %s | ESP-NOW channel: %u]\n",
    role == HIP ? "HIP" : "WRIST", WiFi.macAddress().c_str(), NodeConfig::RADIO_CHANNEL);
  uint8_t ownMac[6];
  WiFi.macAddress(ownMac);
  const uint8_t* peer = peerMac();
  const uint8_t zero[6] = {};
  if (!memcmp(peer, zero, 6) || (peer[0] & 1) || !memcmp(peer, ownMac, 6)) {
    Serial.println("[ESP-NOW disabled: enter the OTHER board's Wi-Fi STA MAC in ExerciseConfig.h]");
    return false;
  }
  if (esp_wifi_set_channel(NodeConfig::RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
  // Keep STA mode (required for ESP-NOW RX + Classic BT coexistence).
  // Do not join a router: it could change our fixed channel.
  incoming = xQueueCreate(12, sizeof(LinkPacket));
  outgoing = xQueueCreate(12, sizeof(LinkPacket));
  completion = xQueueCreate(1, sizeof(uint8_t));
  if (!incoming || !outgoing || !completion || esp_now_init() != ESP_OK) return false;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, peer, 6);
  info.channel = NodeConfig::RADIO_CHANNEL;
  info.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&info) != ESP_OK ||
      esp_now_register_recv_cb(onReceive) != ESP_OK ||
      esp_now_register_send_cb(onSent) != ESP_OK) return false;
  started = true;
  Serial.println("[ESP-NOW ready; activation, mode and LED events; no heartbeats or sensor streaming]");
  return true;
}

bool NodeRadio::ready() { return started; }

void NodeRadio::send(const LinkPacket& packet) {
  if (started) xQueueSend(outgoing, &packet, 0);
}

bool NodeRadio::receive(LinkPacket& packet) {
  return started && xQueueReceive(incoming, &packet, 0) == pdTRUE;
}

void NodeRadio::update() {
  if (!started) return;
  uint8_t done;
  if (xQueueReceive(completion, &done, 0) == pdTRUE) sending = false;
  if (sending) return;
  LinkPacket packet;
  if (xQueueReceive(outgoing, &packet, 0) != pdTRUE) return;
  // One outstanding send at a time. NodeSession retries activation requests
  // until a matching application acknowledgement arrives or the request fails.
  sending = esp_now_send(peerMac(), reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)) == ESP_OK;
}
