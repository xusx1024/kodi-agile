if(CORE_SYSTEM_NAME STREQUAL windows OR CORE_SYSTEM_NAME STREQUAL windowsstore)
  list(APPEND SOURCES DXVAHD.cpp)
  list(APPEND HEADERS DXVAHD.h)
endif()

if(VAAPI_FOUND)
  if(OPENGL_FOUND)
    list(APPEND SOURCES RendererVAAPIGL.cpp
                        VaapiEGL.cpp)
    list(APPEND HEADERS RendererVAAPIGL.h
                        VaapiEGL.h)
  elseif(OPENGLES_FOUND)
    list(APPEND SOURCES RendererVAAPIGLES.cpp)
    list(APPEND HEADERS RendererVAAPIGLES.h)
  endif()
  list(APPEND SOURCES VaapiEGL.cpp)
  list(APPEND HEADERS VaapiEGL.h)
endif()

if(VDPAU_FOUND)
  list(APPEND SOURCES RendererVDPAU.cpp
                      VdpauGL.cpp)
  list(APPEND HEADERS RendererVDPAU.h
                      VdpauGL.h)
endif()

if(CORE_SYSTEM_NAME STREQUAL osx)
  list(APPEND SOURCES RendererVTBGL.cpp)
  list(APPEND HEADERS RendererVTBGL.h)
endif()

if(CORE_SYSTEM_NAME STREQUAL ios)
  list(APPEND SOURCES RendererVTBGLES.cpp)
  list(APPEND HEADERS RendererVTBGLES.h)
endif()

if(MMAL_FOUND)
  list(APPEND SOURCES MMALRenderer.cpp)
  list(APPEND HEADERS MMALRenderer.h)
endif()

if(AML_FOUND)
  list(APPEND SOURCES RendererAML.cpp)
  list(APPEND HEADERS RendererAML.h)
endif()

if(CORE_SYSTEM_NAME STREQUAL android)
  list(APPEND SOURCES RendererMediaCodec.cpp
                      RendererMediaCodecSurface.cpp)
  list(APPEND HEADERS RendererMediaCodec.h
                      RendererMediaCodecSurface.h)
endif()

if(CORE_PLATFORM_NAME_LC STREQUAL gbm AND OPENGLES_FOUND)
  list(APPEND SOURCES RendererDRMPRIME.cpp
                      RendererDRMPRIMEGLES.cpp
                      DRMPRIMEEGL.cpp)
  list(APPEND HEADERS RendererDRMPRIME.h
                      RendererDRMPRIMEGLES.h
                      DRMPRIMEEGL.h)
endif()

# we might want to build on linux systems
# with ENABLE_VDPAU=OFF and ENABLE_VAAPI=OFF
if(SOURCES)
  core_add_library(videorenderers_hwdec)
endif()
