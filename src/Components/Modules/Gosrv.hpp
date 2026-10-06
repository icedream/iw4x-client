#pragma once
#include <string>
#include <vector>

#include "Dvar.hpp"

// gosrv_types.h is a vendored copy of the C types from the gosrv
// project (go-dht-serverlist, pkg/capi/sltypes.h). The actual
// functions come from gosrv.dll (a 32-bit c-shared Go build), which
// the IW4x launcher ships next to the game; this module loads it at
// runtime so the normal build needs no link-time dependency.
#include "gosrv_types.h"

namespace Components
{
  // Gosrv replaces the master.iw4x.io server list with a DHT-based
  // one (go-dht-serverlist). The client side (Browse) is used by
  // ServerList to seed the online list; the server side
  // (StartServer) is used by Dedicated instead of the legacy 2
  // minute master heartbeat. Every failure path degrades to "no
  // servers from the DHT", which the callers handle by falling back
  // to the legacy master (client) or by simply not being listed.
  class Gosrv : public Component
  {
  public:
    Gosrv();

    static bool IsEnabled();
    static bool MasterFallback();

    // Client side: blocking (up to gosrv_browse_timeout_ms plus the
    // one-time startup wait). Returns "host:port" strings; empty on
    // any failure or when the feature is disabled. Call on a worker
    // thread, marshal the result to the client pipeline.
    static std::vector<std::string> Browse();

    // Server side: spawns a worker thread that joins the own DHT
    // and starts the announce loop. Idempotent; no-op unless
    // gosrv_host is set. Call once from Dedicated init.
    static void StartServer();

    // Public like in the sibling components (ServerList, Dedicated):
    // the free-function helpers below read the join/bootstrap dvars.
    static Dvar::Var Enable;
    static Dvar::Var JoinMode;
    static Dvar::Var Bootstrap;
    static Dvar::Var AnchorBootstrap;
    static Dvar::Var OperatorKey;
    static Dvar::Var Version;
    static Dvar::Var Host;
    static Dvar::Var Port;
    static Dvar::Var Name;
    static Dvar::Var KeyFile;
    static Dvar::Var BrowseTimeout;
    static Dvar::Var MasterFallbackVar;
  };
}
