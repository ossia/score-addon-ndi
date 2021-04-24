#include <Ndi/Loader.hpp>

#include <ossia/detail/logger.hpp>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif


namespace Ndi
{

Loader::Loader()
{
  using namespace std::literals;
  std::string ndi_path = NDILIB_LIBRARY_NAME;
  const char* ndi_folder = getenv(NDILIB_REDIST_FOLDER);

#ifdef _WIN32
  if (ndi_folder)
  {
    ndi_path = ndi_folder + "\\"s + NDILIB_LIBRARY_NAME;
  }

  m_ndi_dll = LoadLibraryA(ndi_path.c_str());
  if(!m_ndi_dll)
  {
    ossia::logger().error("No NDI library found on the system.\nPlease install an NDI library, for instance https://code.videolan.org/jbk/libndi");
    return;
  }

  const NDIlib_v4* (*NDIlib_v4_load)(void) = NULL;
  if (m_ndi_dll)
  {
    *((FARPROC*)&NDIlib_v4_load) = GetProcAddress((HMODULE)m_ndi_dll, "NDIlib_v4_load");
  }

  if (!NDIlib_v4_load)
  {
    FreeLibrary((HMODULE)m_ndi_dll);
    m_ndi_dll = nullptr;

    ossia::logger().error("Error while loading NDI. Please reinstall an NDI library.");
    return;
  }
#else
  if (ndi_folder)
  {
    ndi_path = ndi_folder + "/"s + NDILIB_LIBRARY_NAME;
  }

  m_ndi_dll = dlopen(ndi_path.c_str(), RTLD_LOCAL | RTLD_LAZY);
  if(!m_ndi_dll)
  {
    ossia::logger().error("No NDI library found on the system.\nPlease install an NDI library, for instance https://code.videolan.org/jbk/libndi");
    return;
  }

  const NDIlib_v4* (*NDIlib_v4_load)(void) = NULL;
  if (m_ndi_dll)
  {
    *((void**)&NDIlib_v4_load) = dlsym(m_ndi_dll, "NDIlib_v4_load");
  }

  if (!NDIlib_v4_load)
  {
    dlclose(m_ndi_dll);
    m_ndi_dll = nullptr;

    ossia::logger().error("No NDI library found on the system.\nPlease install an NDI library, for instance https://code.videolan.org/jbk/libndi");
    return;
  }
#endif
  m_lib = NDIlib_v4_load();
  if (m_lib)
  {
    if (!m_lib->initialize())
    {
      ossia::logger().error("Error while initializing NDI. Likely a CPU too old to run it.");
      m_lib->destroy();
      m_lib = nullptr;
    }
  }
}

Loader::~Loader()
{
  if (m_lib)
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
