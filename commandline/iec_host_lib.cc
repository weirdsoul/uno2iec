#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "boost/format.hpp"
#include "iec_host_lib.h"

using namespace std::chrono_literals;

// Maximum chars to read looking for '\r'. We want to be able to
// process at least one 1541 sector of data, and some characters
// may be escaped, so we'll look for up to 512 (all escaped) characters
// plus the terminator.
static const size_t kMaxLength = 512 + 1;

// Maximum size of one data packet sent to the Arduino.
static const size_t kMaxSendPacketSize = 256;

static const std::string kConnectionStringPrefix = "connect_arduino:";

// Needs to support host mode.
static const int kMinProtocolVersion = 3;

// Number of tries for successfully reading the connection string prefix.
static const int kNumRetries = 5;

// Config values. These are hardcoded for now and match the defaults of
// the Arduino implementation. We request to be the host, so we specify
// a device number of zero here (which is special cased on the Arduino).
// Device zero (the C64 keyboard) is normally not addressed through the
// IEC bus, so this special casing should be OK.
static const int kDeviceNumber = 0;

static const int kDataPin = 3;
static const int kClockPin = 4;
static const int kAtnPin = 5;
static const int kSrqInPin = 6;
static const int kResetPin = 7;

// Commands supported by the Arduino's serial interface. All of these are
// single character strings.
static const std::string kCmdReset = "r"; // Reset the IEC bus.
static const std::string kCmdOpen = "o";  // Open a channel on a device.
static const std::string kCmdClose = "c"; // Close a channel on a device.
static const std::string kCmdGetData =
    "g"; // Get data from a channel on a device.
static const std::string kCmdPutData =
    "p"; // Put data onto a channel on a device.

static std::string GetPrintableString(const std::string &str) {
  std::string result;
  for (const auto &c : str) {
    switch (c) {
    case '\r':
      result += "\\r";
      break;
    case '\n':
      result += "\\n";
      break;
    default:
      if (c < 32) {
        result += (boost::format("#%u") % c).str();
      } else {
        result += c;
      }
    }
  }
  return result;
}

IECBusConnection::IECBusConnection(int arduino_fd, LogCallback log_callback)
    : arduino_fd_(arduino_fd),
      arduino_writer_(std::make_unique<BufferedReadWriter>(arduino_fd)),
      log_callback_(log_callback) {
  // Ignore broken pipes. They may just happen.
  signal(SIGPIPE, SIG_IGN);
  assert(pipe(tthread_pipe_) == 0);
}

IECBusConnection::~IECBusConnection() {
  // It doesn't really matter what we write here, but writing
  // to tthread_pipe_ will tell the background thread to shutdown.
  char tsym = 't';
  assert(write(tthread_pipe_[1], &tsym, 1) == 1);
  if (response_thread_.joinable()) {
    // Step response processing.
    response_thread_.join();
  }

  if (arduino_fd_ != -1) {
    close(arduino_fd_);
    arduino_fd_ = -1;
  }
  close(tthread_pipe_[0]);
  close(tthread_pipe_[1]);
}

bool IECBusConnection::Reset(IECStatus *status) {
  auto f = RequestResult();
  if (!arduino_writer_->WriteString(kCmdReset, status)) {
    return false;
  }
  // Sleep for a bit to give the drive time to reset.
  std::this_thread::sleep_for(2s);
  auto r = f.get();
  if (r.second.ok()) {
    return true;
  } else {
    *status = r.second;
    return false;
  }
}

bool IECBusConnection::OpenChannel(char device_number, char channel,
                                   const std::string &cmd_string,
                                   IECStatus *status) {
  auto f = RequestResult();
  std::string request_string = kCmdOpen + device_number + channel +
                               static_cast<char>(cmd_string.size()) +
                               cmd_string;
  if (!arduino_writer_->WriteString(request_string, status)) {
    return false;
  }
  auto r = f.get();
  if (r.second.ok()) {
    return true;
  } else {
    *status = r.second;
    return false;
  }
}

bool IECBusConnection::ReadFromChannel(char device_number, char channel,
                                       std::string *result, IECStatus *status) {
  auto f = RequestResult();
  std::string request_string = kCmdGetData + device_number + channel;
  if (!arduino_writer_->WriteString(request_string, status)) {
    return false;
  }
  auto r = f.get();
  if (r.second.ok()) {
    *result = r.first;
    return true;
  } else {
    *status = r.second;
    return false;
  }
}

bool IECBusConnection::WriteToChannel(char device_number, char channel,
                                      const std::string &data_string,
                                      IECStatus *status) {
  // Empty string, we're done.
  if (data_string.empty())
    return true;

  size_t curr_pos = 0;
  while (curr_pos < data_string.size()) {
    auto f = RequestResult();
    size_t to_write =
        std::min(data_string.size() - curr_pos, kMaxSendPacketSize);
    std::string request_string = kCmdPutData + device_number + channel +
                                 static_cast<char>(to_write) +
                                 data_string.substr(curr_pos, to_write);
    if (!arduino_writer_->WriteString(request_string, status)) {
      return false;
    }
    auto r = f.get();
    if (!r.second.ok()) {
      *status = r.second;
      return false;
    }
    curr_pos += to_write;
  }
  return true;
}

bool IECBusConnection::CloseChannel(char device_number, char channel,
                                    IECStatus *status) {
  auto f = RequestResult();
  std::string request_string = kCmdClose + device_number + channel;
  if (!arduino_writer_->WriteString(request_string, status)) {
    return false;
  }
  auto r = f.get();
  if (r.second.ok()) {
    return true;
  } else {
    *status = r.second;
    return false;
  }
}

bool IECBusConnection::Initialize(IECStatus *status) {
  std::string connection_string;
  for (int i = 0; i < kNumRetries; ++i) {
    if (!arduino_writer_->ReadTerminatedString('\r', kMaxLength,
                                               &connection_string, status)) {
      return false;
    }
    if (connection_string.size() >= kConnectionStringPrefix.size() &&
        connection_string.substr(0, kConnectionStringPrefix.size()) ==
            kConnectionStringPrefix) {
      break;
    } else if (i >= (kNumRetries - 1)) {
      SetError(IECStatus::CONNECTION_FAILURE,
               std::string("Unknown protocol response: '") +
                   GetPrintableString(connection_string) + "'",
               status);
      return false;
    } else {
      log_callback_('W', "CLIENT",
                    (boost::format("Malformed connection string '%s'") %
                     connection_string)
                        .str());
    }
  }
  int protocol_version = 0;
  if (sscanf(connection_string.substr(kConnectionStringPrefix.size()).c_str(),
             "%i", &protocol_version) <= 0 ||
      protocol_version < kMinProtocolVersion) {
    SetError(IECStatus::CONNECTION_FAILURE,
             std::string("Unsupported protocol: '") + connection_string + "'",
             status);
    return false;
  }
  time_t unix_time = time(nullptr);
  struct tm local_time;
  localtime_r(&unix_time, &local_time);

  // Now talk back to the Arduino, communicating our configuration.
  auto config_string =
      boost::format("OK>%u|%u|%u|%u|%u|%u|%u-%u-%u.%u:%u:%u\r") %
      kDeviceNumber % kAtnPin % kClockPin % kDataPin % kResetPin % kSrqInPin %
      (local_time.tm_year + 1900) % (local_time.tm_mon + 1) %
      local_time.tm_mday % local_time.tm_hour % local_time.tm_min %
      local_time.tm_sec;
  if (!arduino_writer_->WriteString(config_string.str(), status)) {
    return false;
  }

  // Start our response thread.
  response_thread_ = std::thread(&IECBusConnection::ProcessResponses, this);

  return true;
}

std::future<std::pair<std::string, IECStatus>>
IECBusConnection::RequestResult() {
  response_promise_ = std::promise<std::pair<std::string, IECStatus>>();
  return response_promise_.get_future();
}

void IECBusConnection::ProcessResponses() {

  fd_set rfds;
  // Remember the last response we received. We'll return it along
  // with the status once we have it.
  std::string last_response;
  while (true) {
    if (!arduino_writer_->HasBufferedData()) {
      // If we don't have any more buffered data, see if we can get more data
      // from the file descriptor or if our thread should be cancelled.
      FD_ZERO(&rfds);
      FD_SET(arduino_fd_, &rfds);
      FD_SET(tthread_pipe_[0], &rfds);

      int select_result = select(std::max(arduino_fd_, tthread_pipe_[0]) + 1,
                                 &rfds, nullptr, nullptr, nullptr);
      assert(select_result > 0);
      // Terminate the thread if we received the terminate signal. Don't bother
      // to actually read from the pipe, we don't really care.
      if (FD_ISSET(tthread_pipe_[0], &rfds))
        return;
    }

    std::string read_string;
    IECStatus status;

    if (!arduino_writer_->ReadUpTo(1, 1, &read_string, &status)) {
      log_callback_('E', "CLIENT", status.message);
      return;
    }
    switch (read_string[0]) {
    case '!':
      // Debug channel configuration.
      if (!arduino_writer_->ReadTerminatedString('\r', kMaxLength, &read_string,
                                                 &status)) {
        log_callback_('E', "CLIENT", status.message);
        return;
      }
      if (read_string.size() < 2) {
        log_callback_(
            'E', "CLIENT",
            (boost::format("Malformed channel configuration string '%s'") %
             read_string)
                .str());
        return;
      }
      debug_channel_map_[read_string[0]] = read_string.substr(1);
      break;
    case 'D':
      // Standard debug message.
      if (!arduino_writer_->ReadTerminatedString('\r', kMaxLength, &read_string,
                                                 &status)) {
        log_callback_('E', "CLIENT", status.message);
        return;
      }
      if (read_string.size() < 3 ||
          debug_channel_map_.count(read_string[1]) == 0) {
        // Print the malformed message, but don't terminate execution.
        log_callback_('E', "CLIENT",
                      (boost::format("Malformed debug message '%s'") %
                       GetPrintableString(read_string))
                          .str());
        return;
      }
      log_callback_(read_string[0], debug_channel_map_[read_string[1]],
                    read_string.substr(2));
      break;
    case 'r': {
      // Standard data response message.
      if (!arduino_writer_->ReadTerminatedString('\r', kMaxLength, &read_string,
                                                 &status)) {
        log_callback_('E', "CLIENT", status.message);
        return;
      }
      std::string unescaped_response;
      if (!UnescapeString(read_string, &unescaped_response, &status)) {
        log_callback_('E', "CLIENT", status.message);
        return;
      }
      last_response = unescaped_response;
    } break;
    case 's': {
      // Standard status response message.
      if (!arduino_writer_->ReadTerminatedString('\r', kMaxLength, &read_string,
                                                 &status)) {
        log_callback_('E', "CLIENT", status.message);
        return;
      }
      IECStatus iecStatus;
      if (!read_string.empty()) {
        // We can use the status string directly, it isn't escaped.
        SetError(IECStatus::IEC_CONNECTION_FAILURE, read_string, &iecStatus);
      }
      response_promise_.set_value(
          std::pair<std::string, IECStatus>(last_response, iecStatus));
      // Forget the last response so we won't return it again.
      last_response.clear();
    } break;
    default:
      // Ignore all other messages.
      log_callback_('E', "CLIENT",
                    (boost::format("Unknown response msg type %#x") %
                     static_cast<int>(read_string[0]))
                        .str());
      return;
    }
  }
}

IECBusConnection *IECBusConnection::Create(int arduino_fd,
                                           LogCallback log_callback,
                                           IECStatus *status) {
  if (arduino_fd == -1) {
    return nullptr;
  }
  auto conn = std::make_unique<IECBusConnection>(arduino_fd, log_callback);
  if (!conn->Initialize(status)) {
    return nullptr;
  }
  return conn.release();
}

static bool ConfigureSerial(int fd, int speed, IECStatus *status) {
  struct termios tty;
  if (tcgetattr(fd, &tty) == -1) {
    SetErrorFromErrno(IECStatus::CONNECTION_FAILURE, "tcgetattr", status);
    return false;
  }
  speed_t speed_constant = B0;
#define SPEED_MAP(s)                                                           \
  case s:                                                                      \
    speed_constant = B##s;                                                     \
    break
  switch (speed) {
    SPEED_MAP(0);
    SPEED_MAP(50);
    SPEED_MAP(75);
    SPEED_MAP(110);
    SPEED_MAP(134);
    SPEED_MAP(150);
    SPEED_MAP(200);
    SPEED_MAP(300);
    SPEED_MAP(600);
    SPEED_MAP(1200);
    SPEED_MAP(2400);
    SPEED_MAP(4800);
    SPEED_MAP(9600);
    SPEED_MAP(19200);
    SPEED_MAP(38400);
    SPEED_MAP(57600);
    SPEED_MAP(115200);
    SPEED_MAP(230400);
  default:
    SetError(IECStatus::CONNECTION_FAILURE,
             (boost::format("Unknown speed setting: #%u baud") % speed).str(),
             status);
    return false;
  }
#undef SPEED_MAP
  if (cfsetospeed(&tty, speed_constant) == -1) {
    SetErrorFromErrno(IECStatus::CONNECTION_FAILURE, "cfsetospeed", status);
    return false;
  }
  if (cfsetispeed(&tty, speed_constant) == -1) {
    SetErrorFromErrno(IECStatus::CONNECTION_FAILURE, "cfsetispeed", status);
    return false;
  }

  tty.c_cflag |= (CLOCAL | CREAD); /* ignore modem controls */
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;      /* 8-bit characters */
  tty.c_cflag &= ~PARENB;  /* no parity bit */
  tty.c_cflag &= ~CSTOPB;  /* only need 1 stop bit */
  tty.c_cflag &= ~CRTSCTS; /* no hardware flowcontrol */

  /* setup for non-canonical mode */
  tty.c_iflag &=
      ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

  tty.c_oflag &= ~OPOST;

  /* fetch bytes as they become available */
  tty.c_cc[VMIN] = 1;
  tty.c_cc[VTIME] = 1;

  if (tcsetattr(fd, TCSANOW, &tty) == -1) {
    SetErrorFromErrno(IECStatus::CONNECTION_FAILURE, "tcsetattr", status);
    return false;
  }
  return true;
}

IECBusConnection *IECBusConnection::Create(const std::string &device_file,
                                           int speed, LogCallback log_callback,
                                           IECStatus *status) {
  int fd = open(device_file.c_str(), O_RDWR | O_NONBLOCK);
  if (fd == -1) {
    SetErrorFromErrno(IECStatus::CONNECTION_FAILURE,
                      "open(\"" + device_file + "\")", status);
    return nullptr;
  }

  // Configure serial port to 1200 baud to make the Arduino reset.
  if (!ConfigureSerial(fd, 1200, status)) {
    return nullptr;
  }

  // Wait for the Arduino to reset, then flush everything that was sent or
  // received.
  usleep(1000 * 1000);

  // Now configure to the desired speed.
  if (!ConfigureSerial(fd, speed, status)) {
    return nullptr;
  }
  if (tcflush(fd, TCIFLUSH) == -1) {
    SetErrorFromErrno(IECStatus::CONNECTION_FAILURE, "tcflush", status);
    return nullptr;
  }

  return Create(fd, log_callback, status);
}
