#ifndef SERVER_PROTOCOL_SELFTEST_H
#define SERVER_PROTOCOL_SELFTEST_H

#include "server/server.h"

#include <stdbool.h>

// Packet codec and NetLoopback checks that need no world. Logs one
// "PROTOCOL_TEST: ok <check>" line per passing check and stops at the first
// failure, logging "PROTOCOL_TEST: FAIL <check>".
bool RunProtocolCodecSelfTest(void);

// End-to-end checks against an initialised server: input over NetLoopback,
// player state, block deltas, chunk replication, malformed input and the
// delta-overflow chunk resend. Binds and unbinds its own loopback channel.
bool RunProtocolServerSelfTest(ServerState *Server);

#endif
