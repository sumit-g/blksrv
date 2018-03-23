#include "nbd_loopback_server.h"

#include <set>
#include <list>
#include <thread>

#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/fs.h>

using namespace std;

namespace {
class ServerInfo;

mutex g_nbd_lock;
uint32_t g_num_nbds = 0;
set<uint32_t> g_nbds_avail;
list<unique_ptr<ServerInfo>> g_server_list;

// Kernel thread state
#define KTHR_STATE_INIT		0
#define KTHR_STATE_RUN		1
#define KTHR_STATE_EXIT		2
class ServerInfo {
 public:
  ServerInfo();
  ~ServerInfo() { Cleanup(); }
  void Cleanup();

  unique_ptr<NbdServer> server;
  unique_ptr<thread> kernel_thread;
  int nbd_num = -1;
  int devfd = -1;
  // socks[0] is for kernel and socks[1] is for NbdServer.
  int socks[2] = { -1, -1 };
  unsigned kernel_thread_state = KTHR_STATE_INIT;
  unsigned kernel_thread_error = 0;
  unsigned being_polled:1,  // 1 = polling thread is holding a ref.
           shutting_down:1,
           in_global_list:1,  // Not sure if this is needed.
           rsvd:1;
  string nbd_node;
};

ServerInfo::ServerInfo() {
  nbd_num = -1;
  devfd = -1;
  kernel_thread_state = KTHR_STATE_INIT;
  kernel_thread_error = 0;
  socks[0] = socks[1] = -1;
  being_polled = shutting_down = in_global_list = 0;
}

// When Cleanup is called, ideally, no nbd callbacks should be
// pending, otherwise this function will be stuck waiting
// for pending callbacks.
void ServerInfo::Cleanup() {
  unique_lock<mutex> l(g_nbd_lock);
  shutting_down = 1;
  while (being_polled) {
    l.unlock();
    usleep(1000);
    l.lock();
  }
  l.unlock();
  server.reset();
  if (socks[0] >= 0) close(socks[0]);
  if (socks[1] >= 0) close(socks[1]);
  socks[0] = -1;
  socks[1] = -1;
  if (kernel_thread.get()) {
    while (kernel_thread_state != KTHR_STATE_EXIT)
      usleep(1000);
    kernel_thread->join();
    kernel_thread.reset();
  }
  if (devfd >= 0) {
    ioctl(devfd, NBD_CLEAR_QUE);
    ioctl(devfd, NBD_CLEAR_SOCK);
    close(devfd);
    devfd = -1;
  }
  if (nbd_num > 0) {
    l.lock();
    g_nbds_avail.insert(nbd_num);
    nbd_num = -1;
    l.unlock();
  }
}

}  // anonymouns namespace

int NbdLoopbackInit() {
  // Make sure NBD is loaded.
  system("/sbin/modprobe nbd >/dev/null 2>&1");
  g_nbds_avail.clear();
  uint32_t ndx = 0;
  string nbd_class_path = "/sys/class/block/nbd";
  while (1) {
    string nbd_path = nbd_class_path + to_string(ndx);
    if (access(nbd_path.c_str(), F_OK) != 0)
      break;
    int fd = open((nbd_path + "/size").c_str(), O_RDONLY);
    if (fd >= 0) {
      char size_str[16] = {16};
      int ret = read(fd, size_str, 16);
      close(fd);
      if ((ret > 0) && (strtoul(size_str, nullptr, 0) == 0))
        g_nbds_avail.insert(ndx);
    } else {
      g_nbds_avail.insert(ndx);
    }
    ndx++;
    
    // Put some upper bound on it in case of bugs.
    if (ndx > 10000) {
      return EIO;
    }
  }
  if (ndx == 0) {
    return ENOENT;
  }
  g_num_nbds = ndx;
  return 0;
}

namespace {

// Kernel thread does not own ServerInfo
void NbdKernelThread(ServerInfo *info) {
  if (ioctl(info->devfd, NBD_SET_SOCK, info->socks[0]) < 0) {
    info->kernel_thread_error = errno;
    info->kernel_thread_state = KTHR_STATE_EXIT;
    return;
  }
  if (ioctl(info->devfd, NBD_SET_FLAGS, NBD_FLAG_SEND_FUA|
            NBD_FLAG_SEND_TRIM|NBD_FLAG_SEND_FLUSH) < 0) {
    info->kernel_thread_error = errno;
    info->kernel_thread_state = KTHR_STATE_EXIT;
    return;
  }
  info->kernel_thread_error = 0;
  info->kernel_thread_state = KTHR_STATE_RUN;
  ioctl(info->devfd, NBD_DO_IT);
  ioctl(info->devfd, NBD_CLEAR_QUE);
  ioctl(info->devfd, NBD_CLEAR_SOCK);
  info->kernel_thread_state = KTHR_STATE_EXIT;
}

}  // anonymous namespace

int NbdLoopbackStart(
    const NbdParams &params, int *nbd_num, string *ret_nbd_dev) {

  unique_ptr<ServerInfo> info(new ServerInfo());

  if (g_num_nbds == 0) {
    return ENOENT;
  }
  // Validate blocksize.
  uint32_t bsize = params.block_size;
  if (((bsize & (bsize - 1)) != 0) || (bsize < 512) || (bsize > 65536)) {
    return EINVAL;
  }
  unique_lock<mutex> l(g_nbd_lock);
  if (g_nbds_avail.size() == 0) {
    return ENOENT;
  }
  info->nbd_num = *nbd_num;
  if (info->nbd_num >= 0) {
    if (g_nbds_avail.find(info->nbd_num) == g_nbds_avail.end()) {
      return ENOENT;
    }
  } else {
    info->nbd_num = *g_nbds_avail.begin();
  }
  g_nbds_avail.erase(info->nbd_num);
  l.unlock();

  *nbd_num = info->nbd_num;
  *ret_nbd_dev = string("/dev/nbd") + to_string(info->nbd_num);
  info->nbd_node = *ret_nbd_dev;
  info->devfd = open(ret_nbd_dev->c_str(), O_RDWR);
  if (info->devfd < 0) {
    return errno;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, info->socks) < 0) {
    return errno;
  }
  if (ioctl(info->devfd, NBD_CLEAR_SOCK, 0) < 0) {
    return errno;
  }
  if (ioctl(info->devfd, NBD_SET_BLKSIZE, bsize) < 0) {
    return errno;
  }
  if (ioctl(info->devfd, NBD_SET_SIZE_BLOCKS, params.num_blocks) < 0) {
    return errno;
  }
  info->kernel_thread.reset(new thread(NbdKernelThread, info.get()));
  while (info->kernel_thread_state == KTHR_STATE_INIT) {
    this_thread::yield();
  }
  if (info->kernel_thread_state == KTHR_STATE_EXIT) {
    return EIO;
  }
  assert(info->kernel_thread_state == KTHR_STATE_RUN);
  ioctl(info->devfd, BLKBSZSET, bsize);
  auto st = NbdServer::New(info->socks[1], params, &info->server);
  if (st != 0) {
    return st;
  }
  info->socks[1] = -1;  // this is now owned by server.
  l.lock();
  info->in_global_list = 1;
  g_server_list.push_back(move(info));

  return 0;
}

void NbdLoopbackStop(const string &nbd_node) {
  unique_lock<mutex> l(g_nbd_lock);
  ServerInfo *info = nullptr;
  for (auto it = g_server_list.begin(); it != g_server_list.end(); it++) {
    if ((*it)->nbd_node == nbd_node) {
      (*it)->shutting_down = 1;
      while ((*it)->being_polled) {
        l.unlock();
        usleep(1000);
        l.lock();
      }
      info = it->release();
      g_server_list.erase(it);
      break;
    }
  }
  l.unlock();
  delete info;  // The destructor takes care of the rest.
}

void NbdLoopbackPoll() {
  static int loop_count = 0;
  bool config_poll = false;
  unique_lock<mutex> l(g_nbd_lock);
  // g_nbd_lock also protects loop_count.
  if (++loop_count == 500) {
    loop_count = 0;
    config_poll = true;
  }
  for (auto it = g_server_list.begin(); it != g_server_list.end(); it++) {
    ServerInfo *info = it->get();
    if (info->shutting_down || info->being_polled)
      continue;
    info->being_polled = 1;
    l.unlock();
    info->server->DataPoll();
    if (config_poll)
      info->server->ConfigPoll();
    l.lock();
    info->being_polled = 0;
  }
}
