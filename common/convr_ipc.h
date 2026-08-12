// The single place where Windows and Linux differ. Everything above this
// header sees one server and one client that move TreadmillPacket frames.
//
// Transport: named pipe on Windows, Unix domain socket on Linux.
// Threading: the server owns a background thread doing blocking accept/read
// and publishes the newest packet; callers poll it from whatever thread they
// like (the driver polls from SteamVR's RunFrame).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "convr_protocol.h"

namespace convr {

// Resolves the platform-specific endpoint path for `name`. Exposed so the UI
// and the README can show the user exactly what they should be looking at.
std::string EndpointPath(const char* name);

class IpcServer {
 public:
  IpcServer() = default;
  ~IpcServer();

  IpcServer(const IpcServer&) = delete;
  IpcServer& operator=(const IpcServer&) = delete;

  // Starts the listener thread. Returns false only if the endpoint could not
  // be created at all; a running server with no client attached is normal.
  bool Start(const char* name = kEndpointName);
  void Stop();

  // Copies out the most recently received packet. Returns false if nothing has
  // arrived yet. `age_seconds` reports how long ago it landed, so the caller
  // can decide when to treat a silent companion as "stopped".
  bool LatestPacket(TreadmillPacket* out, double* age_seconds = nullptr) const;

  bool client_connected() const { return client_connected_.load(); }
  uint64_t packets_received() const { return packets_received_.load(); }
  // Extra companions turned away while one was already connected.
  uint64_t rejected_clients() const { return rejected_clients_.load(); }

 private:
  void ThreadMain();

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> client_connected_{false};
  std::atomic<uint64_t> packets_received_{0};
  std::atomic<uint64_t> rejected_clients_{0};

  mutable std::mutex packet_mutex_;
  TreadmillPacket latest_{};
  bool have_packet_ = false;
  double latest_time_ = 0.0;

  std::string endpoint_;
  // Platform handles live in the .cpp; an intptr_t keeps this header clean of
  // windows.h. -1 means "not open" on both platforms.
  intptr_t listen_handle_ = -1;
  intptr_t client_handle_ = -1;
};

class IpcClient {
 public:
  IpcClient() = default;
  ~IpcClient();

  IpcClient(const IpcClient&) = delete;
  IpcClient& operator=(const IpcClient&) = delete;

  // Non-blocking-ish: one connect attempt, returns false if no server is
  // listening. Callers retry on a timer rather than blocking their UI.
  bool Connect(const char* name = kEndpointName);
  void Disconnect();
  bool connected() const { return connected_; }

  // Returns false and disconnects if the server went away, so the caller's
  // next Connect() attempt re-establishes the link.
  bool Send(const TreadmillPacket& packet);

  const std::string& endpoint() const { return endpoint_; }
  const std::string& last_error() const { return last_error_; }

 private:
  bool connected_ = false;
  intptr_t handle_ = -1;
  std::string endpoint_;
  std::string last_error_;
};

}  // namespace convr
