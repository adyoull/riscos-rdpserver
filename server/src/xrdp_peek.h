/* RDPServer: a look inside libxrdp (xrdp_peek.c).
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#ifndef RDPS_XRDP_PEEK_H
#define RDPS_XRDP_PEEK_H
struct xrdp_session;
int rdps_negotiation_failure (struct xrdp_session *session);
/* Forgets the drawing orders' state (the last order and its fields,
   which later orders are sent as changes to): the client forgets its own
   at each activation. libxrdp does it only when the server resizes the
   client, not when the client starts again by itself. */
void rdps_orders_reset (struct xrdp_session *session);
#endif
