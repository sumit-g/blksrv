// Create a loopback device using NBD. One end of the loopback
// is a nbd node (/dev/nbdx) and the other end is a bunch of
// callbacks.
#ifndef _NBD_LOOPBACK_SERVER_H_
#define _NBD_LOOPBACK_SERVER_H_

#include "nbd_server.h"

// This has to be called before any of the other functions.
// Returns 0 on success, errno on error.
int NbdLoopbackInit();
// if nbd_num < 0, an appropriate num is picked and returned.
// Returns 0 on success, errno on error.
int NbdLoopbackStart(
    const NbdParams &params, int *nbd_num, string *ret_nbd_dev);
void NbdLoopbackStop(const string &nbd_node);
void NbdLoopbackPoll();

#endif  // _NBD_LOOPBACK_SERVER_H_
