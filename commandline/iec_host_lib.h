// The IEC host library provides functionality to talk to
// devices on an IEC bus connected via Arduino.
// The host library assumes the function of a host, hence the name.

#ifndef IEC_HOST_LIB_H
#define IEC_HOST_LIB_H

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "utils.h"

class IECBusConnection {
public:
  typedef std::function<void(char level, const std::string &channel,
                             const std::string &message)>
      LogCallback;

  // Instantiate an IECBusConnection object. The arduino_fd parameter is
  // used to specify a file descriptor that will be used for bidirectional
  // communication with an arduino connected to the IEC bus and speaking the
  // uno2iec protocol. The log_callback function will be invoked for all
  // log messages received from the Arduino (called from a separate thread).
  // Use Create() methods below instead of instantiating directly!
  IECBusConnection(int arduino_fd, LogCallback log_callback);

  // Reset the IEC bus by pulling the reset line to low. Returns true on
  // success. In case of an error, status will be set to an appropriate error
  // status.
  virtual bool Reset(IECStatus *status);

  // Open channel on the device with the specific device_number. The optional
  // data_string specifies data to send to the channel, e.g. a filename.
  // Its maximum size is 255 bytes. Returns true on success. In case of an
  // error, status will be set to an appropriate error status.
  virtual bool OpenChannel(char device_number, char channel,
                           const std::string &data_string, IECStatus *status);

  // Read from device_number, channel until an EOI is found or an error
  // occurred.
  // Sets *result to be the resulting string and returns true. If an error is
  // found, sets status and returns false.
  virtual bool ReadFromChannel(char device_number, char channel,
                               std::string *result, IECStatus *status);

  // Write data_string to device_number, channel. Returns true if successful,
  // sets status and returns false otherwise. If data_string has > 256 bytes,
  // multiple requests will be generated.
  virtual bool WriteToChannel(char device_number, char channel,
                              const std::string &data_string,
                              IECStatus *status);

  // Close channel on the device with the specific device_number.
  // Returns true on success. In case of an error, status will be
  // set to an appropriate error status.
  virtual bool CloseChannel(char device_number, char channel,
                            IECStatus *status);

  // Create IECBusConnection instance using the specified device_file and serial
  // port speed. If log_callback is specified, the function will be called for
  // every log message received from the Arduino. Returns nullptr in case of a
  // problem and sets status. Otherwise, ownership of the IECBusConnection
  // instance is transferred to the caller.
  static IECBusConnection *Create(const std::string &device_file, int speed,
                                  LogCallback log_callback, IECStatus *status);

  // Create IECBusConnection instance based on the specified arduino_fd, which
  // must me ready to use. Ownership of the file description is passed to the
  // IECBusConnection instance. If log_callback is specified, the function will
  // be called for every log message received from the Arduino. Returns nullptr
  // in case of a problem and sets status. Otherwise, ownership of the
  // IECBusConnection instance is transferred to the caller.
  static IECBusConnection *Create(int arduino_fd, LogCallback log_callback,
                                  IECStatus *status);

  // Free resources such as any owned file descriptors.
  virtual ~IECBusConnection();

  // Initialize the bus connection. To be called immediately after construction.
  // Returns true if successful. In case of error, returns false and sets
  // status.
  bool Initialize(IECStatus *status);

private:
  // RequestResult registers a promise with the background thread and returns
  // a future on it.
  std::future<std::pair<std::string, IECStatus>> RequestResult();

  // Run on the response background thread. Reads from arduino_writer_,
  // calls log_callback_ for log messages and dispatches responses.
  void ProcessResponses();

  // File descriptor used for communication.
  int arduino_fd_;

  // A buffered reader / writer used for communication.
  std::unique_ptr<BufferedReadWriter> arduino_writer_;

  // Callback used to process log messages
  LogCallback log_callback_;

  // Thread processing responses from the Arduino, including log messages.
  std::thread response_thread_;

  // The current response promise. Will be updated for every incoming request.
  std::promise<std::pair<std::string, IECStatus>> response_promise_;

  // Configured and used by the response thread to provide user identifiable
  // debug log channel names.
  std::map<char, std::string> debug_channel_map_;

  // A pipe created in the constructor and used to signal to the background
  // thread that it should terminate execution.
  int tthread_pipe_[2];
};

#endif // IEC_HOST_LIB_H
