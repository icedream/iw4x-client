#include <Utils/InfoString.hpp>

#include "Gosrv.hpp"
#include "Dedicated.hpp"
#include "Components/Loader.hpp"
#include "ServerList.hpp"

#include <Utils/Library.hpp>
#include <Utils/String.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// gosrv.dll is a 32-bit c-shared Go build of
// https://github.com/icedream/go-dht-serverlist (pkg/capi). The
// launcher ships it next to the game; we load it at runtime so the
// build itself has no dependency on it.
#define GOSRV_DLL "gosrv.dll"

namespace
{
  // cgo export symbols are plain C; the prototypes below are the
  // const-correct spelling of the generated header (cgo does not
  // encode const, so this is ABI-compatible).
  using SlApiVersionFn = int(*)();
  using SlLogCbFn = void(*)(int level, const char* msg, void* ud);
  using SlSetLogCbFn = void(*)(SlLogCbFn cb, void* ud);
  using SlLastErrorFn = const char*(*)(void);
  using SlNewFn = void*(*)(const SlConfig* cfg);
  using SlBrowseFn = int(*)(void* h, const char* version, int timeoutMs, SlServer* out, int cap, int* count);
  using SlFreeFn = void(*)(void* h);
  using SlAnnounceFn = SlResult(*)(void* h, const SlServerConfig* cfg);

  struct GosrvApi
  {
    SlApiVersionFn apiVersion = nullptr;
    SlSetLogCbFn setLogCb = nullptr;
    SlLastErrorFn lastError = nullptr;
    SlNewFn new_ = nullptr;
    SlBrowseFn browse = nullptr;
    SlFreeFn free_ = nullptr;
    SlAnnounceFn announce = nullptr;

    bool load()
    {
      Utils::Library lib(GOSRV_DLL, true);
      if (!lib.isValid())
      {
        return false;
      }

      apiVersion = lib.getProc<SlApiVersionFn>("sl_api_version");
      setLogCb = lib.getProc<SlSetLogCbFn>("sl_log_set_cb");
      lastError = lib.getProc<SlLastErrorFn>("sl_last_error");
      new_ = lib.getProc<SlNewFn>("sl_new");
      browse = lib.getProc<SlBrowseFn>("sl_browse");
      free_ = lib.getProc<SlFreeFn>("sl_free");
      announce = lib.getProc<SlAnnounceFn>("sl_announce");

      // The Library object is intentionally destroyed here: the
      // module stays resident (LoadLibrary refcount) and the
      // pointers remain valid for the process lifetime.
      return apiVersion && setLogCb && lastError && new_ && browse && free_ && announce;
    }
  };

  GosrvApi Api;

  // Route gosrv log lines into the IW4x console. Called from Go
  // worker threads; Logger::MessagePrint already enqueues off-main
  // messages. The message is passed through verbatim (never as a
  // format string).
  void gosrvLogCallback(int level, const char* msg, void*)
  {
    if (!msg)
    {
      return;
    }

    const auto prefix = level == SL_LOG_ERROR ? "^1[gosrv] " : (level == SL_LOG_WARN ? "^3[gosrv] " : "[gosrv] ");
    Components::Logger::Print(std::string(prefix) + msg);
  }

  // A NULL-terminated C array from a vector of std::strings. The
  // backing storage lives in the vectors held by the caller for the
  // duration of the call.
  std::vector<const char*> toCStrings(const std::vector<std::string>& in)
  {
    std::vector<const char*> out;
    out.reserve(in.size() + 1);
    for (const auto& s : in)
    {
      out.push_back(s.c_str());
    }
    out.push_back(nullptr);
    return out;
  }

  // Split a comma-separated dvar string into addresses, trimming
  // whitespace and skipping empty parts.
  std::vector<std::string> splitAddrs(const char* raw)
  {
    std::vector<std::string> out;
    if (!raw || !*raw)
    {
      return out;
    }

    auto parts = Utils::String::Split(std::string(raw), ',');
    for (auto& p : parts)
    {
      Utils::String::Trim(p);
      if (!p.empty())
      {
        out.push_back(p);
      }
    }
    return out;
  }

  // The anchor bootstrap addresses and the operator public key
  // (hex) are infrastructure constants. They are shipped in the
  // launcher/game distribution and may be overridden by dvar. Fill
  // the defaults once the gosrv operator infrastructure for IW4x is
  // live; while they are empty, join mode 1 is disabled (the client
  // falls back to the legacy master, the server simply does not
  // announce).
  constexpr auto* DEFAULT_ANCHOR_ADDRS = "";
  constexpr auto* DEFAULT_OPERATOR_KEY_HEX = "";
  constexpr auto* DEFAULT_PREFIX = "gosrv/iw4x/v1/";
  constexpr auto* DEFAULT_ANCHOR_PREFIX = "gosrv/anchor/v1/";

  // Client-side state: one gosrv client per process (the C ABI
  // contract). Init runs on a worker thread because sl_new blocks
  // while it dials the DHT.
  std::once_flag clientInitFlag;
  std::atomic<void*> clientHandle{ nullptr };
  std::atomic<bool> clientReady{ false };

  void ensureClient()
  {
    std::call_once(clientInitFlag, []()
    {
      std::thread([]()
      {
        if (!Api.load())
        {
          Components::Logger::Print("gosrv: {} not found next to the game; the DHT server list is disabled (using the legacy master server)\n", GOSRV_DLL);
          return;
        }

        if (Api.apiVersion() != 1)
        {
          Components::Logger::Print("gosrv: unsupported API version {} (want 1); the DHT server list is disabled\n", Api.apiVersion());
          return;
        }

        Api.setLogCb(gosrvLogCallback, nullptr);

        SlConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.prefix = DEFAULT_PREFIX;
        cfg.deployment = 0;
        cfg.browse_timeout_ms = 0;
        cfg.max_servers = 0;

        auto joinMode = Gosrv::JoinMode.get<int>();
        auto bootstrap = splitAddrs(Gosrv::Bootstrap.get<const char*>());
        auto anchor = splitAddrs(Gosrv::AnchorBootstrap.get<const char*>());
        auto opKeyHex = std::string(Gosrv::OperatorKey.get<const char*>());
        if (opKeyHex.empty())
        {
          opKeyHex = DEFAULT_OPERATOR_KEY_HEX;
        }
        if (anchor.empty())
        {
          anchor = splitAddrs(DEFAULT_ANCHOR_ADDRS);
        }

        // Resolve defaults: explicit dvars win, then the shipped
        // constants, then join mode 0 (direct).
        std::vector<unsigned char> opKey;
        auto hexValue = [](char c) -> int
        {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        if (opKeyHex.size() == 64)
        {
          bool ok = true;
          for (size_t i = 0; i < opKeyHex.size(); i += 2)
          {
            auto hi = hexValue(opKeyHex[i]);
            auto lo = hexValue(opKeyHex[i + 1]);
            if (hi < 0 || lo < 0)
            {
              ok = false;
              break;
            }
            opKey.push_back(static_cast<unsigned char>((hi << 4) | lo));
          }
          if (!ok)
          {
            Components::Logger::Print("^3gosrv: gosrv_op_key is not valid hex; ignoring it\n");
            opKey.clear();
          }
        }

        std::vector<std::string> own = bootstrap;
        std::vector<const char*> cOwn = toCStrings(own);
        std::vector<const char*> cAnchor = toCStrings(anchor);

        if (joinMode == 1 && !anchor.empty() && !opKey.empty())
        {
          cfg.join_mode = 1;
          cfg.public_addrs = cAnchor.data();
          cfg.operator_pubkey = opKey.data();
          cfg.public_prefix = DEFAULT_ANCHOR_PREFIX;
          cfg.startup_timeout_ms = 10000;
          // Hardcoded last resort: the direct bootstrap dvar, as a
          // game would ship its own-DHT addresses in its binary.
          if (!cOwn.empty())
          {
            cfg.fallback_addrs = cOwn.data();
            cfg.fallback_count = static_cast<int>(own.size());
          }
        }
        else
        {
          if (joinMode == 1)
          {
            Components::Logger::Print("^3gosrv: anchor addresses/operator key not configured; falling back to direct bootstrap\n");
          }
          cfg.join_mode = 0;
          if (cOwn.empty())
          {
            Components::Logger::Print("^3gosrv: no bootstrap addresses configured (gosrv_bootstrap empty); the DHT server list is disabled\n");
            return;
          }
          cfg.bootstrap_addrs = cOwn.data();
        }

        // cgo export takes non-const pointers; the cast is safe
        // because the library only reads the struct.
        auto* h = Api.new_(reinterpret_cast<SlConfig*>(&cfg));
        if (h == nullptr)
        {
          Components::Logger::Print("^1gosrv: sl_new failed: {}\n", Api.lastError());
          return;
        }

        clientHandle = h;
        clientReady = true;
      })
      .detach();
    });
  }

  bool clientUp(std::chrono::milliseconds wait)
  {
    // Give the one-time init a bounded chance to finish; the worker
    // thread owns all blocking gosrv calls.
    auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (clientReady)
      {
        return true;
      }
      std::this_thread::sleep_for(100ms);
    }
    return clientReady;
  }
}

Dvar::Var Gosrv::Enable;
Dvar::Var Gosrv::JoinMode;
Dvar::Var Gosrv::Bootstrap;
Dvar::Var Gosrv::AnchorBootstrap;
Dvar::Var Gosrv::OperatorKey;
Dvar::Var Gosrv::Version;
Dvar::Var Gosrv::Host;
Dvar::Var Gosrv::Port;
Dvar::Var Gosrv::Name;
Dvar::Var Gosrv::KeyFile;
Dvar::Var Gosrv::BrowseTimeout;
Dvar::Var Gosrv::MasterFallbackVar;

Gosrv::Gosrv()
{
  Events::OnDvarInit([]
    {
      Enable = Dvar::Register<bool>("gosrv_enable", true,
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "Use the gosrv DHT server list in the server browser");
      MasterFallbackVar = Dvar::Register<bool>("gosrv_master_fallback", true,
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "Also query the legacy master server and merge the results");
      JoinMode = Dvar::Register<int>("gosrv_join_mode", 1,
        0, 1,
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "0 = direct bootstrap, 1 = resolve the bootstrap list via the anchor DHT");
      Bootstrap = Dvar::Register<const char*>("gosrv_bootstrap", "",
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "Comma-separated own-DHT bootstrap multiaddrs, each with a /p2p/<peer-id> suffix");
      AnchorBootstrap = Dvar::Register<const char*>("gosrv_anchor_bootstrap", "",
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "Comma-separated anchor-DHT multiaddrs (join_mode 1)");
      OperatorKey = Dvar::Register<const char*>("gosrv_op_key", "",
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "Operator ed25519 public key (hex, 64 chars) for anchor list verification");
      Version = Dvar::Register<const char*>("gosrv_version", "1",
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "Exact game version the DHT list is filtered on");
      Host = Dvar::Register<const char*>("gosrv_host", "",
        Game::DVAR_NONE,
        "Server: public address clients can reach (empty = do not announce)");
      Port = Dvar::Register<int>("gosrv_port", 28960,
        1, 65535, Game::DVAR_NONE,
        "Server: game port to announce");
      Name = Dvar::Register<const char*>("gosrv_name", "",
        Game::DVAR_NONE,
        "Server: display name (empty = use sv_hostname)");
      KeyFile = Dvar::Register<const char*>("gosrv_key_file", "gosrv.key",
        Game::DVAR_NONE,
        "Server: ed25519 key file for announcements (created if missing)");
      BrowseTimeout = Dvar::Register<int>("gosrv_browse_timeout_ms", 8000,
        500, 30000,
        Dedicated::IsEnabled() ? Game::DVAR_NONE : Game::DVAR_ARCHIVE,
        "Client: per-browse deadline in milliseconds");
    });
}

bool Gosrv::IsEnabled()
{
  return Enable.get<bool>();
}

bool Gosrv::MasterFallback()
{
  return MasterFallbackVar.get<bool>();
}

std::vector<std::string> Gosrv::Browse()
{
  std::vector<std::string> out;
  if (!IsEnabled())
  {
    return out;
  }

  ensureClient();
  if (!clientUp(std::chrono::seconds(5)))
  {
    return out;
  }

  SlServer servers[256];
  int count = -1;
  auto rc = Api.browse(clientHandle, Version.get<const char*>(),
                       BrowseTimeout.get<int>(), servers, 256, &count);
  if (rc != SL_OK && rc != SL_TRUNCATED)
  {
    Components::Logger::Print("^3gosrv: browse failed: {}\n", Api.lastError());
    return out;
  }

  for (int i = 0; i < count && i < 256; ++i)
  {
    if (servers[i].host[0] == '\0' || servers[i].port == 0)
    {
      continue;
    }
    out.push_back(Utils::String::VA("{}:{}", servers[i].host, servers[i].port));
  }

  return out;
}

void Gosrv::StartServer()
{
  if (Dedicated::IsEnabled() && Host.get<const char*>()[0] == '\0')
  {
    Components::Logger::Print("^3gosrv: gosrv_host is not set; this server will not announce to the DHT server list\n");
    return;
  }

  std::thread([]()
  {
    if (!Api.load())
    {
      Components::Logger::Print("^1gosrv: {} not found next to the server; cannot announce\n", GOSRV_DLL);
      return;
    }

    Api.setLogCb(gosrvLogCallback, nullptr);

    // The server announces directly on the own DHT; its bootstrap
    // addresses come from the hosting configuration (gosrv_bootstrap).
    auto bootstrap = splitAddrs(Bootstrap.get<const char*>());
    std::vector<const char*> cOwn = toCStrings(bootstrap);
    if (cOwn.empty())
    {
      Components::Logger::Print("^1gosrv: gosrv_bootstrap is empty; cannot announce to the DHT server list\n");
      return;
    }

    SlConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.prefix = DEFAULT_PREFIX;
    cfg.deployment = 0;
    cfg.join_mode = 0;
    cfg.bootstrap_addrs = cOwn.data();
    cfg.startup_timeout_ms = 30000;

    auto* h = Api.new_(reinterpret_cast<SlConfig*>(&cfg));
    if (h == nullptr)
    {
      Components::Logger::Print("^1gosrv: sl_new failed: {}\n", Api.lastError());
      return;
    }

    auto name = std::string(Name.get<const char*>());
    if (name.empty())
    {
      name = std::string((*Game::sv_hostname)->current.string);
    }

    SlServerConfig sc;
    std::memset(&sc, 0, sizeof(sc));
    sc.key_file = KeyFile.get<const char*>();
    sc.host = Host.get<const char*>();
    sc.port = static_cast<unsigned short>(Port.get<int>());
    sc.version = Version.get<const char*>();
    sc.name = name.c_str();
    sc.proto = "udp";
    sc.refresh_ms = 0;
    sc.startup_timeout_ms = 30000;

    auto rc = Api.announce(h, &sc);
    if (rc != SL_OK)
    {
      Components::Logger::Print("^1gosrv: sl_announce failed ({}): {}\n", static_cast<int>(rc), Api.lastError());
    }
    else
    {
      Components::Logger::Print("gosrv: announcing this server to the DHT server list as {}:{} (v{})\n",
                    Host.get<const char*>(), Port.get<int>(), Version.get<const char*>());
    }

    // The C ABI keeps the client alive until sl_free; a dedicated
    // server runs for its whole lifetime, so nothing to do here.
  })
  .detach();
}
