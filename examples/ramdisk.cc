// A test program using nbd loopback server.
//
// Allocates a 100MB block and exposes that as a ramdisk using nbd.

#include "nbd_loopback_server.h"

#include <thread>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

constexpr uint64_t kMemSize = 100 * 1024 * 1024;
constexpr uint32_t kBlockSize = 4096;
constexpr uint64_t kNumBlocks = kMemSize/kBlockSize;
char *mem = nullptr;

void *rd_alloc_mem(unsigned size) {
  return malloc(size);
}

void rd_free_mem(void *ptr) {
  free(ptr);
}

void rd_read(void *arg, NbdCmd *cmd) {
  if ((cmd->io_offset + cmd->io_size) > kMemSize) {
    cmd->ret_error = ENOSPC;
  } else {
    bcopy(mem + cmd->io_offset, cmd->data_buf, cmd->io_size);
    cmd->ret_error = 0;
  }
  cmd->completion_cb(cmd);
}

void rd_write(void *arg, NbdCmd *cmd) {
  if ((cmd->io_offset + cmd->io_size) > kMemSize) {
    cmd->ret_error = ENOSPC;
  } else {
    bcopy(cmd->data_buf, mem + cmd->io_offset, cmd->io_size);
    cmd->ret_error = 0;
  }
  cmd->completion_cb(cmd);
}

void rd_flush(void *arg, NbdCmd *cmd) {
  cmd->ret_error = 0;
  cmd->completion_cb(cmd);
}

void rd_trim(void *arg, NbdCmd *cmd) {
  cmd->ret_error = 0;
  cmd->completion_cb(cmd);
}

int main() {
  int st = NbdLoopbackInit();
  if (st != 0) {
    fprintf(stderr, "Failed to init loopback : %s\n", strerror(st));
    exit(1);
  }
  mem = (char *)malloc(kMemSize);
  if (mem == nullptr) {
    fprintf(stderr, "Unable to allocate memory\n");
    exit(1);
  }
  NbdParams params;
  params.block_size = kBlockSize;
  params.num_blocks = kNumBlocks;
  params.arg = nullptr;
  params.alloc_data_mem = rd_alloc_mem;
  params.free_data_mem = rd_free_mem;
  params.read = rd_read;
  params.write = rd_write;
  params.trim = rd_trim;
  params.flush = rd_flush;
  params.disconnect = nullptr;

  int nbd_num = -1;
  string nbd_dev;
  st = NbdLoopbackStart(params, &nbd_num, &nbd_dev);
  if (st != 0) {
    fprintf(stderr, "Failed to start loopback : %s\n", strerror(st));
    exit(1);
  }
  bool terminate = false;
  thread t([&terminate]() {
      while(!terminate) {
        NbdLoopbackPoll();
        usleep(100);
      }
    });
  
  printf("Started NBD, dev = %s\n", nbd_dev.c_str());
  printf("Press any key to stop ...\n");
  getchar();
  NbdLoopbackStop(nbd_dev);
  terminate = true;
  t.join();
  free(mem);

  return 0;
}
