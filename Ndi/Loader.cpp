#include <ossia/detail/logger.hpp>

#include <Ndi/HxDecoder.hpp>
#include <Ndi/Loader.hpp>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <QFile>
namespace Ndi
{

Loader::Loader()
{
  using namespace std::literals;
  const char* ndi_folder = getenv(NDILIB_REDIST_FOLDER);
  if(!ndi_folder)
  {
    ndi_folder = getenv("NDI_RUNTIME_DIR_V6");
    if(!ndi_folder)
    {
      ndi_folder = getenv("NDI_RUNTIME_DIR_V5");
    }
  }
  std::string ndi_path = NDILIB_LIBRARY_NAME;

#ifdef _WIN32
  if(ndi_folder)
  {
    ndi_path = ndi_folder + "\\"s + NDILIB_LIBRARY_NAME;
  }

  m_ndi_dll = LoadLibraryA(ndi_path.c_str());
  if(!m_ndi_dll)
  {
    ossia::logger().error(
        "No NDI library found on the system.\nPlease install an NDI library, for "
        "instance https://code.videolan.org/jbk/libndi");
    return;
  }

  const NDIlib_v5* (*NDIlib_v5_load)(void) = NULL;
  if(m_ndi_dll)
  {
    *((FARPROC*)&NDIlib_v5_load) = GetProcAddress((HMODULE)m_ndi_dll, "NDIlib_v5_load");
  }

  if(!NDIlib_v5_load)
  {
    FreeLibrary((HMODULE)m_ndi_dll);
    m_ndi_dll = nullptr;

    ossia::logger().error("Error while loading NDI. Please reinstall an NDI library.");
    return;
  }
#elif defined(__APPLE__)
  if(ndi_folder)
    ndi_path = ndi_folder + "/libndi.dylib"s;
  else if(QFile::exists("/usr/local/lib/libndi.dylib"))
    ndi_path = "/usr/local/lib/libndi.dylib";
  else
    ndi_path = "libndi.dylib";

  m_ndi_dll = dlopen(ndi_path.c_str(), RTLD_LOCAL | RTLD_LAZY);
  if(!m_ndi_dll)
  {
    ossia::logger().error(
        "No NDI library found on the system.\nPlease install an NDI library, for "
        "instance https://code.videolan.org/jbk/libndi");
    return;
  }

  const NDIlib_v5* (*NDIlib_v5_load)(void) = NULL;
  if(m_ndi_dll)
  {
    *((void**)&NDIlib_v5_load) = dlsym(m_ndi_dll, "NDIlib_v5_load");
  }

  if(!NDIlib_v5_load)
  {
    dlclose(m_ndi_dll);
    m_ndi_dll = nullptr;

    ossia::logger().error(
        "No NDI library found on the system.\nPlease install an NDI library, for "
        "instance https://code.videolan.org/jbk/libndi");
    return;
  }
#else
  // Newest first, everywhere we look. The version matters: a v5 runtime cannot
  // receive HDR at all and substitutes a placeholder frame for it, silently.
  static constexpr auto sonames
      = {"libndi.so.8", "libndi.so.7", "libndi.so.6", "libndi.so.5"};

  // The environment's folder, if it named one.
  //
  // This loop used to assign ndi_path only when ndi_folder was set, so without
  // one it dlopen()ed NDILIB_LIBRARY_NAME four times over -- the soname
  // preference did nothing, and the search fell to whatever version the
  // bundled header happened to name. On a machine with both runtimes installed
  // that is how score ended up on v5 with an SDK 6 sitting in /opt.
  if(ndi_folder)
  {
    for(auto ndi_name : sonames)
    {
      ndi_path = ndi_folder + "/"s + ndi_name;
      m_ndi_dll = dlopen(ndi_path.c_str(), RTLD_LOCAL | RTLD_LAZY);
      if(m_ndi_dll)
      {
        ossia::logger().info("Found NDI: {}", ndi_path);
        break;
      }
    }
  }

  // The SDK's own install directories, before the bare soname. An NDI SDK
  // unpacked in /opt is not on the loader path, so dlopen("libndi.so.6") fails
  // by name and the search below lands on whatever older runtime the
  // distribution happens to ship -- silently. That costs more than it sounds:
  // a v5 runtime cannot receive HDR at all, and hands over a placeholder frame
  // instead, with no error anywhere.
  if(!m_ndi_dll)
  {
    static constexpr auto sdk_dirs = {
        "/opt/sdk/ndi/lib/x86_64-linux-gnu",
        "/opt/sdk/ndi/lib/aarch64-rpi4-linux-gnueabi",
        "/usr/local/lib",
        "/Library/NDI SDK for Apple/lib/macOS",
    };
    for(auto dir : sdk_dirs)
    {
      for(auto ndi_name : sonames)
      {
        const auto candidate = dir + "/"s + ndi_name;
        if((m_ndi_dll = dlopen(candidate.c_str(), RTLD_LOCAL | RTLD_LAZY)))
        {
          ndi_path = candidate;
          ossia::logger().info("Found NDI: {}", candidate);
          break;
        }
      }
      if(m_ndi_dll)
        break;
    }
  }

  // By bare soname, for a runtime the dynamic loader already knows about, then
  // the name the bundled header was built against, then the unversioned link.
  if(!m_ndi_dll)
  {
    for(auto ndi_name : sonames)
    {
      if((m_ndi_dll = dlopen(ndi_name, RTLD_LOCAL | RTLD_LAZY)))
      {
        ndi_path = ndi_name;
        ossia::logger().info("Found NDI: {}", ndi_name);
        break;
      }
    }
  }

  if(!m_ndi_dll)
  {
    for(auto ndi_name : {NDILIB_LIBRARY_NAME, "libndi.so"})
    {
      if((m_ndi_dll = dlopen(ndi_name, RTLD_LOCAL | RTLD_LAZY)))
      {
        ndi_path = ndi_name;
        ossia::logger().info("Found NDI: {}", ndi_name);
        break;
      }
    }
  }

  if(!m_ndi_dll)
  {
    ossia::logger().error(
        "No NDI library found on the system.\nPlease install an NDI library, for "
        "instance https://code.videolan.org/jbk/libndi");
    return;
  }

  const NDIlib_v5* (*NDIlib_v5_load)(void) = NULL;
  if(m_ndi_dll)
  {
    *((void**)&NDIlib_v5_load) = dlsym(m_ndi_dll, "NDIlib_v5_load");
  }

  if(!NDIlib_v5_load)
  {
    dlclose(m_ndi_dll);
    m_ndi_dll = nullptr;

    ossia::logger().error(
        "No NDI library found on the system.\nPlease install an NDI library, for "
        "instance https://code.videolan.org/jbk/libndi");
    return;
  }
#endif

  m_lib = NDIlib_v5_load();
  if(m_lib)
  {
    if(!m_lib->initialize())
    {
      ossia::logger().error(
          "Error while initializing NDI. Likely a CPU too old to run it.");
      m_lib->destroy();
      m_lib = nullptr;
    }
    else if(const char* v = m_lib->version())
    {
      // Which runtime we ended up on, every time. "NDI is installed but score
      // found an older one first" is otherwise a diagnosis nobody can make from
      // the outside: the symptom is a black or frozen picture on HDR sources
      // and nothing at all in the log.
      m_version = v;
      m_path = ndi_path;
      ossia::logger().info("NDI runtime: {}", v);
      if(!supportsHDR())
        ossia::logger().info(
            "NDI runtime is older than version 6: HDR sources will arrive as a "
            "placeholder frame. Set NDI_RUNTIME_DIR_V6 to an SDK 6 runtime to "
            "receive them.");
    }
  }
}

bool Loader::supportsHDR() const noexcept
{
  return ndiVersionSupportsHDR(m_version);
}

Loader::~Loader()
{
  if(m_lib)
  {
    m_lib->destroy();
  }

  if(m_ndi_dll)
  {
#if defined(_WIN32)
    FreeLibrary((HMODULE)m_ndi_dll);
#else
    dlclose(m_ndi_dll);
#endif
  }
}


bool hxDecoderAvailable(const Loader& ndi) noexcept
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
    || defined(__NetBSD__)
  if(!ndi.available())
    return true;

  const auto libs = hxDecoderLibs(ndiVersionMajor(ndi.version()));
  if(!libs.known())
    return true;

  std::string dir;
  if(const auto slash = ndi.path().find_last_of('/'); slash != std::string::npos)
    dir = ndi.path().substr(0, slash + 1);

  auto loadable = [&dir](const char* soname) {
    const std::string name{soname};
    const auto dot = name.find(".so.");
    const std::string priv = dot == std::string::npos
                                 ? name
                                 : name.substr(0, dot) + "-ndi" + name.substr(dot);

    // The runtime searches its own directory before the loader path, and
    // prefers the privately named copy in each, so a system whose only usable
    // FFmpeg sits next to libndi still decodes.
    for(const auto& candidate : {dir + priv, dir + name, priv, name})
    {
      if(void* h = dlopen(candidate.c_str(), RTLD_LOCAL | RTLD_LAZY))
      {
        dlclose(h);
        return true;
      }
    }
    return false;
  };

  return loadable(libs.avcodec) && loadable(libs.avutil);
#else
  return true;
#endif
}
}
