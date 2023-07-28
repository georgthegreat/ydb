#pragma once

#include "public.h"

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Creates a channel that routes all requests to the local RPC server.
IChannelPtr CreateLocalChannel(IServerPtr server);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
