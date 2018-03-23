#include "nbd_server.h"
#include <fcntl.h>
#include <stddef.h>
#include <endian.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>

#include <thread>
#include <chrono>

namespace {

static constexpr uint32_t kMaxNbdIOSize = 1024 * 1024;
static uint32_t kNbdReqMagic = be32toh(NBD_REQUEST_MAGIC);
static uint32_t kNbdReplyMagic = be32toh(NBD_REPLY_MAGIC);

int fd_set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return flags;
  flags |= O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}

static void NbdCompletionCb(NbdCmd *cmd) {
  cmd->server->CompletionCb(cmd);
}

static NbdCmd *alloc_nbd_cmd_(void *arg) {
  NbdCmd *cmd = new NbdCmd();
  if (cmd) {
    cmd->arg = arg;
    cmd->completion_cb = NbdCompletionCb;
    cmd->reply.magic = kNbdReplyMagic;
  }
  return cmd;
}

static void free_nbd_cmd_(void *arg, NbdCmd *cmd) {
  delete cmd;
}
}  // anonymous namespace

NbdServer::NbdServer(const NbdParams &params) :
    cmd_cache_(alloc_nbd_cmd_, params.arg, free_nbd_cmd_, nullptr,
               offsetof(NbdCmd, link)),
    send_cmds_(offsetof(NbdCmd, link)),
    pending_backend_cmds_(offsetof(NbdCmd, link)) {
  rcv_running_ = false;
  send_running_ = false;
  config_running_ = false;
  rcv_cmd_ = nullptr;
  send_cmd_ = nullptr;
  shutdown_ = false;
  last_config_run_ = 0;
}

NbdServer::~NbdServer() {
  MarkShutdown("Server getting destroyed");
  // There is a tiny chance of race condition between checking
  // shutdown_ and setting *_running_ flag among all the poll
  // functions. Initial sleep closes that window for all practical
  // purposes. Also the caller mostly is not going to destroy this
  // object without finishing the pollers (and stopping future poll
  // calls) and that completely eliminates this window.
  do {
    this_thread::sleep_for(chrono::milliseconds(1));
  } while (rcv_running_ || send_running_ || config_running_ ||
           (pending_backend_cmds_.size() > 0));
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  unique_lock<mutex> l(lock_);
  if (rcv_cmd_ != nullptr) {
    if (rcv_cmd_->data_buf != nullptr) {
      params_.free_data_mem(rcv_cmd_->data_buf);
      rcv_cmd_->data_buf = nullptr;
    }
    cmd_cache_.Free(&l, rcv_cmd_);
    rcv_cmd_ = nullptr;
  }
  if (send_cmd_ != nullptr) {
    if (send_cmd_->data_buf != nullptr) {
      params_.free_data_mem(send_cmd_->data_buf);
      send_cmd_->data_buf = nullptr;
    }
    cmd_cache_.Free(&l, send_cmd_);
    send_cmd_ = nullptr;
  }
  NbdCmd *cmd;
  while ((cmd = send_cmds_.PopFront()) != nullptr) {
    if (cmd->data_buf != nullptr) {
      params_.free_data_mem(cmd->data_buf);
      cmd->data_buf = nullptr;
    }
    cmd_cache_.Free(&l, cmd);
  }
  l.unlock();
}

void NbdServer::CompletionCb(NbdCmd *cmd) {
  // Prepare reply, magic is set already.
  cmd->reply.error = htobe32(cmd->ret_error);
  bcopy(cmd->req.handle, cmd->reply.handle, 8);
  cmd->cur_state = NBDCMD_STATE_SEND_REPLY;
  cmd->cur_io_ptr = (void *)&cmd->reply;
  cmd->io_size_remaining = sizeof(cmd->reply);
  unique_lock<mutex> l(lock_);
  pending_backend_cmds_.Remove(cmd);
  send_cmds_.PushBack(cmd);
}

bool NbdServer::CheckShutdown(string *reason) {
  unique_lock<mutex> l(lock_);
  if (shutdown_) {
    *reason = shutdown_reason_;
    return true;
  }
  return false;
}

// static
int NbdServer::New(
    int sockfd,
    const NbdParams &params,
    unique_ptr<NbdServer> *ret_server) {
  unique_ptr<NbdServer> server(new NbdServer(params));

  server->fd_ = sockfd;
  if (fd_set_nonblock(server->fd_) != 0) {
    return errno;
  }
  server->params_ = params;  // Object copy.

  *ret_server = move(server);
  return 0;
}

void NbdServer::MarkShutdown(const string &reason) {
  // Dont remark shutdown
  unique_lock<mutex> l(lock_);
  if (shutdown_) return;
  shutdown_ = true;
  shutdown_reason_ = reason;
}

void NbdServer::PostRcvdCmd() {
  // Assumed to be called from Rcv Poller
  assert(rcv_running_ == true);
  NbdCmd *cmd = rcv_cmd_;
  rcv_cmd_ = nullptr;
  cmd->cur_state = NBDCMD_STATE_CMD_SUBMITTED;
  if (cmd->req.type != NBD_CMD_DISC) {
    unique_lock<mutex> l(lock_);
    pending_backend_cmds_.PushBack(cmd);
  }
  switch (cmd->req.type) {
    case NBD_CMD_READ:
      params_.read(cmd->arg, cmd);
      break;
    case NBD_CMD_WRITE:
      params_.write(cmd->arg, cmd);
      break;
    case NBD_CMD_DISC:
      if (params_.disconnect)
        params_.disconnect(cmd->arg, cmd);
      MarkShutdown("Disconnect received");
      {
        unique_lock<mutex> l(lock_);
        cmd_cache_.Free(&l, cmd);
      }
      break;
    case NBD_CMD_FLUSH:
      params_.flush(cmd->arg, cmd);
      break;
    case NBD_CMD_TRIM:
      params_.trim(cmd->arg, cmd);
      break;
    default:
      cmd->ret_error = EINVAL;
      CompletionCb(cmd);
  }  // switch (cmd->req.type)
}

void NbdServer::PollRecv() {
  if (shutdown_)
    return;
  if (rcv_cmd_ == nullptr) {
    unique_lock<mutex> l(lock_);
    rcv_cmd_ = cmd_cache_.Alloc(&l);
    l.unlock();
    if (rcv_cmd_ == nullptr)
      return;
    rcv_cmd_->Reset();
    rcv_cmd_->server = this;
  }
  assert(rcv_cmd_->io_size_remaining > 0);
  ssize_t ret = read(fd_, rcv_cmd_->cur_io_ptr, rcv_cmd_->io_size_remaining);
  if (ret <= 0) {
    if (ret < 0) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        return;
    }
    MarkShutdown((ret == 0) ?
                 string("Remote end closed connection during read") :
                 string("Failed to read from socket"));
    return;
  }
  rcv_cmd_->io_size_remaining -= ret;
  if (rcv_cmd_->io_size_remaining != 0) {
    rcv_cmd_->cur_io_ptr = (void *)(((char *)rcv_cmd_->cur_io_ptr) + ret);
    return;
  }
  if (rcv_cmd_->cur_state == NBDCMD_STATE_RCV_WRITE_DATA) {
    PostRcvdCmd();
    return;
  }
  assert(rcv_cmd_->cur_state == NBDCMD_STATE_RCV_REQ);
  rcv_cmd_->req.type = be32toh(rcv_cmd_->req.type);
  if (rcv_cmd_->req.type & NBD_CMD_FLAG_FUA) {
    rcv_cmd_->fua = 1;
  } else {
    rcv_cmd_->fua = 0;
  }
  rcv_cmd_->req.type &= 0xFFFF; // Mask off flags.
  if ((rcv_cmd_->req.magic != kNbdReqMagic) ||
      (rcv_cmd_->req.type > NBD_CMD_TRIM)) {
    MarkShutdown("Invalid cmd received");
    return;
  }
  rcv_cmd_->io_offset = be64toh(rcv_cmd_->req.from);
  rcv_cmd_->io_size = be32toh(rcv_cmd_->req.len);
  if ((rcv_cmd_->req.type == NBD_CMD_READ) ||
      (rcv_cmd_->req.type == NBD_CMD_WRITE)) {
    rcv_cmd_->io_size_remaining = rcv_cmd_->io_size;
    if ((rcv_cmd_->io_size_remaining == 0) ||
        (rcv_cmd_->io_size_remaining > kMaxNbdIOSize)) {
      rcv_cmd_->ret_error = EINVAL;
      CompletionCb(rcv_cmd_);
      rcv_cmd_ = nullptr;
      return;
    }
    rcv_cmd_->cur_io_ptr = rcv_cmd_->data_buf =
        params_.alloc_data_mem(rcv_cmd_->io_size_remaining);
    if (rcv_cmd_->data_buf == nullptr) {
      MarkShutdown("Failed to allocate DMA memory");
      return;
    }
  }
  if (rcv_cmd_->req.type != NBD_CMD_WRITE) {
    PostRcvdCmd();
    return;
  }
  // Write command, start receiving data.
  rcv_cmd_->cur_state = NBDCMD_STATE_RCV_WRITE_DATA;
}

void NbdServer::PollSend() {
  if (send_cmd_ == nullptr) {
    // Do an early check to avoid the lock.
    if (send_cmds_.size() == 0) return;
    unique_lock<mutex> l(lock_);
    send_cmd_ = send_cmds_.PopFront();
    if (send_cmd_ == nullptr)
      return;
  }
  ssize_t ret = write(fd_, send_cmd_->cur_io_ptr,
                      send_cmd_->io_size_remaining);
  if (ret <= 0) {
    if (ret < 0) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        return;
    }
    MarkShutdown((ret == 0) ?
                 string("Remote end closed connection during write") :
                 string("Failed to write to socket"));
    return;
  }
  send_cmd_->io_size_remaining -= ret;
  if (send_cmd_->io_size_remaining != 0) {
    send_cmd_->cur_io_ptr = (void *)(((char *)send_cmd_->cur_io_ptr) + ret);
    return;
  }
  if ((send_cmd_->cur_state == NBDCMD_STATE_SEND_READ_DATA) ||
      (send_cmd_->ret_error != 0) ||
      (send_cmd_->req.type != NBD_CMD_READ) ||
      (send_cmd_->req.len == 0)) {
    if (send_cmd_->data_buf) {
      params_.free_data_mem(send_cmd_->data_buf);
      send_cmd_->data_buf = nullptr;
    }
    unique_lock<mutex> l(lock_);
    cmd_cache_.Free(&l, send_cmd_);
    send_cmd_ = nullptr;
    return;
  }
  // Send read data.
  assert(send_cmd_->cur_state == NBDCMD_STATE_SEND_REPLY);
  send_cmd_->cur_state = NBDCMD_STATE_SEND_READ_DATA;
  send_cmd_->cur_io_ptr = send_cmd_->data_buf;
  send_cmd_->io_size_remaining = be32toh(send_cmd_->req.len);
}

bool NbdServer::ConfigPoll(time_t t) {
  if (shutdown_)
    return false;
  bool flg = false;
  if (!shutdown_ && config_running_.compare_exchange_weak(flg, true)) {
    if ((last_config_run_ - t) > 0) {
      last_config_run_ = t;
      unique_lock<mutex> l(lock_);
      cmd_cache_.HouseKeeping(&l, t);
    }
    config_running_ = false;
  }
  return !shutdown_;
}

bool NbdServer::DataPoll() {
  if (shutdown_)
    return false;
  bool flg = false;
  if (!shutdown_ && rcv_running_.compare_exchange_weak(flg, true)) {
    PollRecv();
    rcv_running_ = false;
    flg = false;
  }
  if (!shutdown_ && send_running_.compare_exchange_weak(flg, true)) {
    PollSend();
    send_running_ = false;
  }
  return !shutdown_;
}
