#include <ossia/detail/logger.hpp>

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
  for(auto ndi_name : {"libndi.so.8", "libndi.so.7", "libndi.so.6", "libndi.so.5"})
  {
    if(ndi_folder)
    {
      ndi_path = ndi_folder + "/"s + ndi_name;
    }
    m_ndi_dll = dlopen(ndi_path.c_str(), RTLD_LOCAL | RTLD_LAZY);
    if(m_ndi_dll)
    {
      ossia::logger().info("Found NDI: {}", ndi_path);
      break;
    }
  }

  if(!m_ndi_dll)
  {
    for(auto ndi_name :
        {"libndi.so.8", "libndi.so.7", "libndi.so.6", "libndi.so.5", "libndi.so"})
    {
      if((m_ndi_dll = dlopen(ndi_name, RTLD_LOCAL | RTLD_LAZY)))
      {
        ossia::logger().info("Found NDI: {}", ndi_path);
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
  }
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

}
