/* RDPServer: what the server needs to know from inside libxrdp that its
   public interface (libxrdpinc.h) doesn't give.
   Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE). */
#include "libxrdp.h"
#include "xrdp_peek.h"

/* Why the X.224 negotiation refused the client ([MS-RDPBCGR] 2.2.1.2.2
   failureCode: SSL_REQUIRED_BY_SERVER and so on), or 0. */
int
rdps_negotiation_failure (struct xrdp_session *session)
{
  struct xrdp_rdp *rdp = session ? (struct xrdp_rdp *) session->rdp : NULL;
  if (!rdp || !rdp->sec_layer || !rdp->sec_layer->mcs_layer
      || !rdp->sec_layer->mcs_layer->iso_layer)
    return 0;
  return rdp->sec_layer->mcs_layer->iso_layer->failureCode;
}

void
rdps_orders_reset (struct xrdp_session *session)
{
  if (session && session->orders)
    xrdp_orders_reset ((struct xrdp_orders *) session->orders);
}
