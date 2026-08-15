// Round-trip check for the IPC shim: starts a server, drives a client through
// connect / send / disconnect / reconnect, and asserts the server sees exactly
// what was sent. Build with -DCONVR_BUILD_TESTS=ON and run convr_ipc_selftest.
#include <stdio.h>

#include <chrono>
#include <thread>

#include "convr_ipc.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) ++g_failures;
}

void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The server publishes from its reader thread, so give it a moment to land.
bool WaitForPacket(const convr::IpcServer& server, uint64_t at_least) {
  for (int i = 0; i < 200; ++i) {
    if (server.packets_received() >= at_least) return true;
    SleepMs(5);
  }
  return false;
}

convr::TreadmillPacket MakePacket(float x, float y, bool button) {
  convr::TreadmillPacket packet = {};
  packet.magic = convr::kProtocolMagic;
  packet.joystick_x = x;
  packet.joystick_y = y;
  packet.button = button ? 1 : 0;
  return packet;
}

}  // namespace

int main() {
  const char* endpoint = "convr_selftest";
  printf("endpoint: %s\n", convr::EndpointPath(endpoint).c_str());

  convr::IpcServer server;
  Check(server.Start(endpoint), "server starts");

  convr::TreadmillPacket received = {};
  Check(!server.LatestPacket(&received), "no packet before any client");
  Check(!server.client_connected(), "no client before connect");

  printf("connect and send:\n");
  convr::IpcClient client;
  Check(client.Connect(endpoint), "client connects");

  Check(client.Send(MakePacket(0.0f, 0.75f, true)), "send succeeds");
  Check(WaitForPacket(server, 1), "server receives the packet");

  double age = 0.0;
  Check(server.LatestPacket(&received, &age), "latest packet is available");
  Check(received.magic == convr::kProtocolMagic, "magic survives the wire");
  Check(received.joystick_y == 0.75f, "y value survives the wire");
  Check(received.button == 1, "button survives the wire");
  Check(age >= 0.0 && age < 2.0, "packet age is sane");
  Check(server.client_connected(), "server reports the client connected");

  printf("streaming:\n");
  const int kBurst = 500;
  bool all_sent = true;
  for (int i = 0; i < kBurst; ++i) {
    all_sent = client.Send(MakePacket(0.0f, i / static_cast<float>(kBurst),
                                      i % 2 == 0)) &&
               all_sent;
  }
  Check(all_sent, "500 back-to-back sends all succeed");
  Check(WaitForPacket(server, kBurst + 1), "server receives the whole burst");
  Check(server.LatestPacket(&received) &&
            received.joystick_y == (kBurst - 1) / static_cast<float>(kBurst),
        "server ends on the newest value (framing stayed aligned)");

  printf("second companion is turned away:\n");
  {
    // Starting the companion twice must not wedge either copy: the newcomer
    // gets a clean disconnect, and the incumbent keeps streaming.
    convr::IpcClient second;
    const bool second_connected = second.Connect(endpoint);
    bool second_send_stopped = false;
    for (int i = 0; i < 100; ++i) {
      if (!second.Send(MakePacket(0.0f, 0.123f, false))) {
        second_send_stopped = true;
        break;
      }
      SleepMs(10);
    }
    Check(second_send_stopped, "second client's sends fail rather than hang");

    const uint64_t before_incumbent = server.packets_received();
    Check(client.Send(MakePacket(0.0f, 0.25f, false)),
          "original client still works");
    Check(WaitForPacket(server, before_incumbent + 1),
          "server still receives from the original client");
    Check(server.LatestPacket(&received) && received.joystick_y == 0.25f,
          "server never mixed in the second client's data");

    // Everything above holds on both platforms. Where they differ is *who*
    // does the refusing, and only the POSIX side has anything to count.
#if defined(_WIN32)
    // A single-instance named pipe rejects the second client in the kernel:
    // CreateFile fails with ERROR_PIPE_BUSY and the server is never involved.
    Check(!second_connected, "the pipe itself refused the second client");
    Check(server.rejected_clients() == 0,
          "server had no rejection to count (the OS handled it)");
#else
    // The listening socket would otherwise leave a second companion stuck
    // unaccepted in the backlog, so the server accepts and closes it itself.
    Check(second_connected, "second client reaches the listening socket");
    Check(server.rejected_clients() > 0, "server counted the rejection");
#endif
  }

  printf("disconnect and reconnect:\n");
  client.Disconnect();
  Check(!client.connected(), "client reports disconnected");
  bool saw_drop = false;
  for (int i = 0; i < 200; ++i) {
    if (!server.client_connected()) {
      saw_drop = true;
      break;
    }
    SleepMs(5);
  }
  Check(saw_drop, "server notices the client went away");

  const uint64_t before = server.packets_received();
  Check(client.Connect(endpoint), "client reconnects to the same server");
  Check(client.Send(MakePacket(0.0f, -0.5f, false)), "send works again");
  Check(WaitForPacket(server, before + 1), "server receives after reconnect");
  Check(server.LatestPacket(&received) && received.joystick_y == -0.5f,
        "reverse direction survives the wire");

  printf("shutdown:\n");
  client.Disconnect();
  server.Stop();
  Check(true, "server stops without hanging");

  // A client with no server must fail rather than block.
  convr::IpcClient orphan;
  Check(!orphan.Connect(endpoint), "connect fails cleanly when nothing listens");
  Check(!orphan.Send(MakePacket(0.0f, 1.0f, false)),
        "send fails cleanly when not connected");

  printf("\n%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
