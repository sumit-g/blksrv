#ifndef _NBD_SERVER_H_
#define _NBD_SERVER_H_

#include "list.h"
#include "cache_allocator.h"
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/nbd.h>
#include <time.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <atomic>
#include <mutex>

class NbdCmd;
class NbdServer;

class NbdParams {
 public:
  // Block device attributes.
  uint32_t block_size;
  uint32_t rsvd;
  uint64_t num_blocks;

  // Argument for the callback functions.
  void *arg;

  // Memory allocation callbacks (sync.)
  function<void*(unsigned)> alloc_data_mem;
  function<void(void*)> free_data_mem;

  // Note: all callbacks, except disconnect, are async.
  // disconnect is optional and if defined, is sync.
  function<void(void *, NbdCmd*)>
      read, write, trim, flush, disconnect;
};

// NbdCmd States.
#define NBDCMD_STATE_RCV_REQ		0
#define NBDCMD_STATE_RCV_WRITE_DATA	1
#define NBDCMD_STATE_CMD_SUBMITTED	2
#define NBDCMD_STATE_SEND_REPLY		3
#define NBDCMD_STATE_SEND_READ_DATA	4

class NbdCmd {
 public:
  NbdCmd() { Reset(); }

  void Reset() {
    cur_state = NBDCMD_STATE_RCV_REQ;
    cur_io_ptr = (void *)&req;
    io_size_remaining = sizeof(req);
    data_buf = nullptr;
    ret_error = 0;
  }
  // Client is not suppose to use link.
  ListLink link;
  struct nbd_request req;
  struct nbd_reply reply;

  // I/O context.
  void *data_buf;
  uint64_t io_offset;
  void *cur_io_ptr;
  unsigned io_size_remaining;
  // Set by implementation to indicate error. 0=no error
  unsigned ret_error;

  // callback context
  NbdServer *server;
  void (*completion_cb)(NbdCmd *cmd);

  uint8_t cur_state;
  uint8_t fua:1;  // FUA bit - Forced unit access.
  uint32_t io_size;

  // From NbdParams
  void *arg;
  // For client to state any per-cmd state.
  void *client_private;
};

class NbdServer {
 public:
  ~NbdServer();

  // Factory method to create a nbd instance. Returns 0 on success,
  // errno in case of error.
  static int New(int sockfd, const NbdParams &params,
                 unique_ptr<NbdServer> *ret_server);

  // Polling routines return false if something has gone wrong.
  // Caller should call CheckShutdown() in that case.
  bool DataPoll();
  bool ConfigPoll(time_t t=time(nullptr));

  // Returns true, if the server has shutdown. Also returns the
  // shutdown reason.
  bool CheckShutdown(string *reason);

  // If this call returns true, then it means that deleting this
  // object would likely not block. Useful when we have to delete
  // this class by a polling thread.
  bool IsDeleteReady() { return shutdown_ && !rcv_running_ &&
                                !send_running_ && !config_running_ &&
                                (pending_backend_cmds_.size() == 0);
                       }

  // Common completion calback from client.
  void CompletionCb(NbdCmd *cmd);

 private:
  NbdServer(const NbdParams &params);
  void PollRecv();
  void PollSend();
  void PostRcvdCmd();
  void MarkShutdown(const string &reason);
  // These atomics allow multiple poll threads to call Poll
  // at the same time.
  atomic<bool> rcv_running_;
  atomic<bool> send_running_;
  atomic<bool> config_running_;
  // rcv_cmd_ is only accessed by PollRecv() which is serialized by
  // rcv_running_ hence no locking is needed for it. But the cache needs
  // a lock between itself and its housekeeping function. So we only use
  // it for cache related calls in rcv path.
  mutex lock_;
  CacheAllocator<NbdCmd> cmd_cache_;
  NbdCmd *rcv_cmd_ = nullptr;
  NbdCmd *send_cmd_ = nullptr;
  List<NbdCmd> send_cmds_;
  List<NbdCmd> pending_backend_cmds_;
  int fd_ = -1;
  NbdParams params_;
  bool shutdown_ = false;
  string shutdown_reason_;
  time_t last_config_run_ = 0;
};

#endif  // _NBD_SERVER_H_
