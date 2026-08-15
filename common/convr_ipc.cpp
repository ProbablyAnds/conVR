#include "convr_ipc.h"

#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace convr {
namespace {

constexpr int kPollTimeoutMs = 100;   // bounds how long Stop() waits
constexpr size_t kReadChunk = 4096;

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#if defined(_WIN32)
constexpr intptr_t kInvalid = -1;
// Bounds a client Send(). POSIX gets the same guarantee from SO_SNDTIMEO.
constexpr int kSendTimeoutMs = 100;
inline HANDLE ToHandle(intptr_t h) { return reinterpret_cast<HANDLE>(h); }
inline intptr_t FromHandle(HANDLE h) { return reinterpret_cast<intptr_t>(h); }

std::string LastErrorString() {
  DWORD code = GetLastError();
  char buffer[256] = {0};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, buffer, sizeof(buffer) - 1, nullptr);
  std::string text(buffer);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return "error " + std::to_string(code) + ": " + text;
}
#else
constexpr intptr_t kInvalid = -1;
#endif

}  // namespace

std::string EndpointPath(const char* name) {
#if defined(_WIN32)
  // Honoured on both platforms so a second instance can be tested side by
  // side. Set it on the companion *and* on whatever launches vrserver.
  if (const char* override_path = getenv("CONVR_IPC_PATH")) {
    return std::string(override_path);
  }
  return std::string("\\\\.\\pipe\\") + name;
#else
  // Deterministic and environment-independent: vrserver may be launched from a
  // very different environment than the companion, so anything derived from
  // XDG_RUNTIME_DIR risks the two processes disagreeing on the path.
  if (const char* override_path = getenv("CONVR_IPC_PATH")) {
    return std::string(override_path);
  }
  return std::string("/tmp/") + name + "-" + std::to_string(getuid()) + ".sock";
#endif
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

IpcServer::~IpcServer() { Stop(); }

bool IpcServer::LatestPacket(TreadmillPacket* out, double* age_seconds) const {
  std::lock_guard<std::mutex> lock(packet_mutex_);
  if (!have_packet_) return false;
  *out = latest_;
  if (age_seconds) *age_seconds = NowSeconds() - latest_time_;
  return true;
}

#if defined(_WIN32)

bool IpcServer::Start(const char* name) {
  if (running_.load()) return true;
  endpoint_ = EndpointPath(name);

  HANDLE pipe = CreateNamedPipeA(
      endpoint_.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1,           // one companion at a time
      0,           // no outbound buffer, this direction is read-only
      kReadChunk,  // inbound buffer
      0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;

  listen_handle_ = FromHandle(pipe);
  running_.store(true);
  thread_ = std::thread(&IpcServer::ThreadMain, this);
  return true;
}

void IpcServer::Stop() {
  if (!running_.exchange(false)) return;
  if (listen_handle_ != kInvalid) {
    // Unblocks any overlapped wait the thread is sitting in.
    CancelIoEx(ToHandle(listen_handle_), nullptr);
  }
  if (thread_.joinable()) thread_.join();
  if (listen_handle_ != kInvalid) {
    CloseHandle(ToHandle(listen_handle_));
    listen_handle_ = kInvalid;
  }
  client_connected_.store(false);
}

void IpcServer::ThreadMain() {
  HANDLE pipe = ToHandle(listen_handle_);
  HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) return;

  std::vector<uint8_t> buffer;
  buffer.reserve(kReadChunk);
  std::vector<uint8_t> chunk(kReadChunk);

  // Waits for a pending overlapped operation, honouring the stop flag. Returns
  // the number of bytes transferred, or -1 on error/cancel.
  auto await = [&](OVERLAPPED* ov) -> int {
    for (;;) {
      DWORD wait = WaitForSingleObject(ov->hEvent, kPollTimeoutMs);
      if (wait == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        if (!GetOverlappedResult(pipe, ov, &transferred, FALSE)) return -1;
        return static_cast<int>(transferred);
      }
      if (wait != WAIT_TIMEOUT) return -1;
      if (!running_.load()) {
        CancelIoEx(pipe, ov);
        DWORD transferred = 0;
        GetOverlappedResult(pipe, ov, &transferred, TRUE);
        return -1;
      }
    }
  };

  while (running_.load()) {
    // --- wait for a companion to connect ---
    OVERLAPPED connect_ov = {};
    connect_ov.hEvent = event;
    ResetEvent(event);

    bool connected = false;
    if (ConnectNamedPipe(pipe, &connect_ov)) {
      connected = true;
    } else {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (err == ERROR_IO_PENDING) {
        connected = await(&connect_ov) >= 0;
      }
    }
    if (!connected) {
      if (!running_.load()) break;
      continue;
    }

    client_connected_.store(true);
    buffer.clear();

    // --- drain frames until the companion goes away ---
    while (running_.load()) {
      OVERLAPPED read_ov = {};
      read_ov.hEvent = event;
      ResetEvent(event);

      DWORD read_now = 0;
      int transferred = -1;
      if (ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()),
                   &read_now, &read_ov)) {
        transferred = static_cast<int>(read_now);
      } else if (GetLastError() == ERROR_IO_PENDING) {
        transferred = await(&read_ov);
      }
      if (transferred <= 0) break;

      buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + transferred);
      while (buffer.size() >= sizeof(TreadmillPacket)) {
        TreadmillPacket packet;
        memcpy(&packet, buffer.data(), sizeof(packet));
        buffer.erase(buffer.begin(), buffer.begin() + sizeof(packet));
        if (packet.magic != kProtocolMagic) continue;
        {
          std::lock_guard<std::mutex> lock(packet_mutex_);
          latest_ = packet;
          have_packet_ = true;
          latest_time_ = NowSeconds();
        }
        packets_received_.fetch_add(1);
      }
    }

    client_connected_.store(false);
    DisconnectNamedPipe(pipe);
  }

  CloseHandle(event);
}

#else  // POSIX

bool IpcServer::Start(const char* name) {
  if (running_.load()) return true;
  endpoint_ = EndpointPath(name);
  if (endpoint_.size() >= sizeof(sockaddr_un::sun_path)) return false;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;

  // A crashed vrserver leaves the socket file behind; it is ours to reclaim.
  unlink(endpoint_.c_str());

  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return false;
  }
  // Only the user running SteamVR should be able to drive their locomotion.
  chmod(endpoint_.c_str(), S_IRUSR | S_IWUSR);
  if (listen(fd, 1) < 0) {
    close(fd);
    unlink(endpoint_.c_str());
    return false;
  }

  listen_handle_ = fd;
  running_.store(true);
  thread_ = std::thread(&IpcServer::ThreadMain, this);
  return true;
}

void IpcServer::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (client_handle_ != kInvalid) {
    close(static_cast<int>(client_handle_));
    client_handle_ = kInvalid;
  }
  if (listen_handle_ != kInvalid) {
    close(static_cast<int>(listen_handle_));
    listen_handle_ = kInvalid;
  }
  if (!endpoint_.empty()) unlink(endpoint_.c_str());
  client_connected_.store(false);
}

void IpcServer::ThreadMain() {
  const int listen_fd = static_cast<int>(listen_handle_);
  std::vector<uint8_t> buffer;
  buffer.reserve(kReadChunk);
  std::vector<uint8_t> chunk(kReadChunk);

  while (running_.load()) {
    // --- wait for a companion to connect ---
    pollfd pfd = {listen_fd, POLLIN, 0};
    int ready = poll(&pfd, 1, kPollTimeoutMs);
    if (ready <= 0) continue;

    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) continue;

    client_handle_ = client_fd;
    client_connected_.store(true);
    buffer.clear();

    // --- drain frames until the companion goes away ---
    while (running_.load()) {
      // Watch the listening socket too. A second companion would otherwise sit
      // unaccepted in the backlog and see its sends fail with EAGAIN, which
      // looks like a broken driver; accepting and closing gives it a clean
      // disconnect it can report and retry from. (Windows gets this for free:
      // a single-instance named pipe refuses the second client outright.)
      pollfd fds[2];
      fds[0] = {client_fd, POLLIN, 0};
      fds[1] = {listen_fd, POLLIN, 0};
      int events = poll(fds, 2, kPollTimeoutMs);
      if (events == 0) continue;
      if (events < 0) {
        if (errno == EINTR) continue;
        break;
      }

      if (fds[1].revents & POLLIN) {
        const int extra = accept(listen_fd, nullptr, nullptr);
        if (extra >= 0) {
          close(extra);
          rejected_clients_.fetch_add(1);
        }
      }

      if (fds[0].revents & (POLLERR | POLLNVAL)) break;
      if (!(fds[0].revents & (POLLIN | POLLHUP))) continue;

      ssize_t got = read(client_fd, chunk.data(), chunk.size());
      if (got == 0) break;  // clean disconnect
      if (got < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        break;
      }

      buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + got);
      while (buffer.size() >= sizeof(TreadmillPacket)) {
        TreadmillPacket packet;
        memcpy(&packet, buffer.data(), sizeof(packet));
        buffer.erase(buffer.begin(), buffer.begin() + sizeof(packet));
        if (packet.magic != kProtocolMagic) continue;
        {
          std::lock_guard<std::mutex> lock(packet_mutex_);
          latest_ = packet;
          have_packet_ = true;
          latest_time_ = NowSeconds();
        }
        packets_received_.fetch_add(1);
      }
    }

    client_connected_.store(false);
    close(client_fd);
    client_handle_ = kInvalid;
  }
}

#endif

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

IpcClient::~IpcClient() { Disconnect(); }

#if defined(_WIN32)

bool IpcClient::Connect(const char* name) {
  if (connected_) return true;
  endpoint_ = EndpointPath(name);

  // FILE_FLAG_OVERLAPPED so Send() can bound how long it waits. A second
  // companion lands here with ERROR_PIPE_BUSY, because the driver's pipe is
  // created with a single instance - that is the clean refusal it reports.
  HANDLE pipe = CreateFileA(endpoint_.c_str(), GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    last_error_ = LastErrorString();
    return false;
  }

  HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    last_error_ = LastErrorString();
    CloseHandle(pipe);
    return false;
  }

  handle_ = FromHandle(pipe);
  event_handle_ = FromHandle(event);
  connected_ = true;
  last_error_.clear();
  return true;
}

void IpcClient::Disconnect() {
  if (handle_ != kInvalid) {
    // Send() always resolves its own overlapped write before returning, so
    // nothing should be in flight here. Belt and braces: this runs before the
    // event handle is closed, so a stray completion cannot signal into it.
    CancelIoEx(ToHandle(handle_), nullptr);
    CloseHandle(ToHandle(handle_));
    handle_ = kInvalid;
  }
  if (event_handle_ != kInvalid) {
    CloseHandle(ToHandle(event_handle_));
    event_handle_ = kInvalid;
  }
  connected_ = false;
}

bool IpcClient::Send(const TreadmillPacket& packet) {
  if (!connected_) return false;

  HANDLE pipe = ToHandle(handle_);
  HANDLE event = ToHandle(event_handle_);
  OVERLAPPED ov = {};
  ov.hEvent = event;
  ResetEvent(event);

  DWORD written = 0;
  if (!WriteFile(pipe, &packet, sizeof(packet), &written, &ov)) {
    if (GetLastError() != ERROR_IO_PENDING) {
      last_error_ = LastErrorString();
      Disconnect();
      return false;
    }
    // Mirrors the POSIX SO_SNDTIMEO: a wedged vrserver must never freeze the
    // companion's UI thread. At 100 Hz a packet is worth 10 ms, so dropping
    // the link and reconnecting beats stalling on a stale one.
    if (WaitForSingleObject(event, kSendTimeoutMs) != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &ov);
      GetOverlappedResult(pipe, &ov, &written, TRUE);
      last_error_ = "send timed out after " +
                    std::to_string(kSendTimeoutMs) + " ms";
      Disconnect();
      return false;
    }
    if (!GetOverlappedResult(pipe, &ov, &written, FALSE)) {
      last_error_ = LastErrorString();
      Disconnect();
      return false;
    }
  }

  if (written != sizeof(packet)) {
    last_error_ = "short write";
    Disconnect();
    return false;
  }
  return true;
}

#else  // POSIX

bool IpcClient::Connect(const char* name) {
  if (connected_) return true;
  endpoint_ = EndpointPath(name);
  if (endpoint_.size() >= sizeof(sockaddr_un::sun_path)) {
    last_error_ = "endpoint path too long";
    return false;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    last_error_ = strerror(errno);
    return false;
  }

  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    last_error_ = strerror(errno);
    close(fd);
    return false;
  }

  // A wedged vrserver must never freeze the companion's UI thread.
  timeval timeout = {0, 100000};  // 100 ms
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  handle_ = fd;
  connected_ = true;
  last_error_.clear();
  return true;
}

void IpcClient::Disconnect() {
  if (handle_ != kInvalid) {
    close(static_cast<int>(handle_));
    handle_ = kInvalid;
  }
  connected_ = false;
}

bool IpcClient::Send(const TreadmillPacket& packet) {
  if (!connected_) return false;
  // MSG_NOSIGNAL: a dead server must return an error, not kill the process.
  ssize_t written = send(static_cast<int>(handle_), &packet, sizeof(packet),
                         MSG_NOSIGNAL);
  if (written != static_cast<ssize_t>(sizeof(packet))) {
    last_error_ = written < 0 ? strerror(errno) : "short write";
    Disconnect();
    return false;
  }
  return true;
}

#endif

}  // namespace convr
