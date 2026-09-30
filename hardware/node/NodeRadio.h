#pragma once

#include "NodeSession.h"

// Callbacks only copy packets to queues. Session logic and printing stay in loop().
namespace NodeRadio {
  bool begin(NodeRole role);
  bool ready();
  void send(const LinkPacket& packet);
  bool receive(LinkPacket& packet);
  void update();
}
