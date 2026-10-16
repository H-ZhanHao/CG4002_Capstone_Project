#pragma once

#include "NodeSession.h"

// Callbacks only copy packets to queues. Session logic stays in the main task.
namespace NodeRadio {
  bool begin(NodeRole role);
  bool ready();
  void send(const LinkPacket& packet);
  bool receive(LinkPacket& packet);
  void update();
}
