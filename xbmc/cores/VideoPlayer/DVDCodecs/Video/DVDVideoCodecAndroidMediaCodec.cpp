/*
 *      Copyright (C) 2013-2017 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

// http://developer.android.com/reference/android/media/MediaCodec.html
//
// Android MediaCodec class can be used to access low-level media codec,
// i.e. encoder/decoder components. (android.media.MediaCodec). Requires SDK21+
//

#include "DVDVideoCodecAndroidMediaCodec.h"

#include <androidjni/ByteBuffer.h>
#include <androidjni/MediaCodecList.h>
#include <androidjni/MediaCodecInfo.h>
#include <androidjni/Surface.h>
#include <androidjni/SurfaceTexture.h>
#include <media/NdkMediaCrypto.h>

#include "Application.h"
#include "ServiceBroker.h"
#include "messaging/ApplicationMessenger.h"
#include "TimingConstants.h"

#include "utils/BitstreamConverter.h"
#include "utils/BitstreamWriter.h"
#include "utils/CPUInfo.h"
#include "utils/log.h"

#include "settings/AdvancedSettings.h"
#include "platform/android/activity/XBMCApp.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "cores/VideoPlayer/DVDDemuxers/DemuxCrypto.h"
#include "platform/android/activity/AndroidFeatures.h"
#include "settings/Settings.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cassert>
#include <memory>

#define XMEDIAFORMAT_KEY_ROTATION "rotation-degrees"
#define XMEDIAFORMAT_KEY_SLICE "slice-height"
#define XMEDIAFORMAT_KEY_CROP_LEFT "crop-left"
#define XMEDIAFORMAT_KEY_CROP_RIGHT "crop-right"
#define XMEDIAFORMAT_KEY_CROP_TOP "crop-top"
#define XMEDIAFORMAT_KEY_CROP_BOTTOM "crop-bottom"

using namespace KODI::MESSAGING;

enum MEDIACODEC_STATES
{
  MEDIACODEC_STATE_UNINITIALIZED,
  MEDIACODEC_STATE_CONFIGURED,
  MEDIACODEC_STATE_FLUSHED,
  MEDIACODEC_STATE_RUNNING,
  MEDIACODEC_STATE_ENDOFSTREAM,
  MEDIACODEC_STATE_ERROR
};

static bool CanSurfaceRenderBlackList(const std::string &name)
{
  // All devices 'should' be capable of surface rendering
  // but that seems to be hit or miss as most odd name devices
  // cannot surface render.
  static const char *cannotsurfacerender_decoders[] = {
    NULL
  };
  for (const char **ptr = cannotsurfacerender_decoders; *ptr; ptr++)
  {
    if (!strnicmp(*ptr, name.c_str(), strlen(*ptr)))
      return true;
  }
  return false;
}

static bool IsBlacklisted(const std::string &name)
{
  static const char *blacklisted_decoders[] = {
    // No software decoders
    "OMX.google",
    // For Rockchip non-standard components
    "AVCDecoder",
    "AVCDecoder_FLASH",
    "FLVDecoder",
    "M2VDecoder",
    "M4vH263Decoder",
    "RVDecoder",
    "VC1Decoder",
    "VPXDecoder",
    // End of Rockchip
    NULL
  };
  for (const char **ptr = blacklisted_decoders; *ptr; ptr++)
  {
    if (!strnicmp(*ptr, name.c_str(), strlen(*ptr)))
      return true;
  }
  return false;
}

static bool IsSupportedColorFormat(int color_format)
{
  static const int supported_colorformats[] = {
    CJNIMediaCodecInfoCodecCapabilities::COLOR_FormatYUV420Planar,
    CJNIMediaCodecInfoCodecCapabilities::COLOR_TI_FormatYUV420PackedSemiPlanar,
    CJNIMediaCodecInfoCodecCapabilities::COLOR_FormatYUV420SemiPlanar,
    CJNIMediaCodecInfoCodecCapabilities::COLOR_QCOM_FormatYUV420SemiPlanar,
    CJNIMediaCodecInfoCodecCapabilities::OMX_QCOM_COLOR_FormatYVU420SemiPlanarInterlace,
    -1
  };
  for (const int *ptr = supported_colorformats; *ptr != -1; ptr++)
  {
    if (color_format == *ptr)
      return true;
  }
  return false;
}

/*****************************************************************************/
/*****************************************************************************/
class CNULL_Listener : public CJNISurfaceTextureOnFrameAvailableListener
{
public:
  CNULL_Listener() : CJNISurfaceTextureOnFrameAvailableListener(jni::jhobject(NULL)) {};

protected:
  virtual void OnFrameAvailable() {};
};

class CDVDMediaCodecOnFrameAvailable : public CEvent, CJNISurfaceTextureOnFrameAvailableListener
{
public:
  CDVDMediaCodecOnFrameAvailable(std::shared_ptr<CJNISurfaceTexture> &surfaceTexture)
  : m_surfaceTexture(surfaceTexture)
  {
    m_surfaceTexture->setOnFrameAvailableListener(*this);
  }

  virtual ~CDVDMediaCodecOnFrameAvailable()
  {
    // unhook the callback
    CNULL_Listener null_listener;
    m_surfaceTexture->setOnFrameAvailableListener(null_listener);
  }

protected:
  virtual void OnFrameAvailable()
  {
    Set();
  }

private:
  std::shared_ptr<CJNISurfaceTexture> m_surfaceTexture;

};

/*****************************************************************************/
/*****************************************************************************/
CDVDMediaCodecInfo::CDVDMediaCodecInfo(
    ssize_t index
  , unsigned int texture
  , AMediaCodec* codec
  , std::shared_ptr<CJNISurfaceTexture> &surfacetexture
  , std::shared_ptr<CDVDMediaCodecOnFrameAvailable> &frameready
)
: m_refs(1)
, m_valid(true)
, m_isReleased(true)
, m_index(index)
, m_texture(texture)
, m_timestamp(0)
, m_codec(codec)
, m_surfacetexture(surfacetexture)
, m_frameready(frameready)
{
  // paranoid checks
  assert(m_index >= 0);
  assert(m_codec != NULL);
}

CDVDMediaCodecInfo::~CDVDMediaCodecInfo()
{
  assert(m_refs == 0);
}

CDVDMediaCodecInfo* CDVDMediaCodecInfo::Retain()
{
  ++m_refs;
  m_isReleased = false;

  return this;
}

long CDVDMediaCodecInfo::Release()
{
  long count = --m_refs;
  if (count == 1)
    ReleaseOutputBuffer(false);
  if (count == 0)
    delete this;

  return count;
}

void CDVDMediaCodecInfo::Validate(bool state)
{
  CSingleLock lock(m_section);

  m_valid = state;
}

bool CDVDMediaCodecInfo::WaitForFrame(int millis)
{
  return m_frameready->WaitMSec(millis);
}

void CDVDMediaCodecInfo::ReleaseOutputBuffer(bool render)
{
  CSingleLock lock(m_section);

  if (!m_valid || m_isReleased)
    return;

  // release OutputBuffer and render if indicated
  // then wait for rendered frame to become available.

  if (render)
    if (m_frameready)
      m_frameready->Reset();

  media_status_t mstat = AMediaCodec_releaseOutputBuffer(m_codec, m_index, render);
  m_isReleased = true;

  if (mstat != AMEDIA_OK)
    CLog::Log(LOGERROR, "CDVDMediaCodecInfo::ReleaseOutputBuffer "
      "error %d in render(%d)", mstat, render);
}

ssize_t CDVDMediaCodecInfo::GetIndex() const
{
  CSingleLock lock(m_section);

  return m_index;
}

int CDVDMediaCodecInfo::GetTextureID() const
{
  // since m_texture never changes,
  // we do not need a m_section lock here.
  return m_texture;
}

void CDVDMediaCodecInfo::GetTransformMatrix(float *textureMatrix)
{
  CSingleLock lock(m_section);

  if (!m_valid)
    return;

  m_surfacetexture->getTransformMatrix(textureMatrix);
}

void CDVDMediaCodecInfo::UpdateTexImage()
{
  CSingleLock lock(m_section);

  if (!m_valid)
    return;

  // updateTexImage will check and spew any prior gl errors,
  // clear them before we call updateTexImage.
  glGetError();

  // this is key, after calling releaseOutputBuffer, we must
  // wait a little for MediaCodec to render to the surface.
  // Then we can updateTexImage without delay. If we do not
  // wait, then video playback gets jerky. To optimize this,
  // we hook the SurfaceTexture OnFrameAvailable callback
  // using CJNISurfaceTextureOnFrameAvailableListener and wait
  // on a CEvent to fire. 50ms seems to be a good max fallback.
  WaitForFrame(50);

  m_surfacetexture->updateTexImage();
  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::Log(LOGERROR, "CDVDMediaCodecInfo::UpdateTexImage updateTexImage:ExceptionCheck");
    xbmc_jnienv()->ExceptionDescribe();
    xbmc_jnienv()->ExceptionClear();
  }

  m_timestamp = m_surfacetexture->getTimestamp();
  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::Log(LOGERROR, "CDVDMediaCodecInfo::UpdateTexImage getTimestamp:ExceptionCheck");
    xbmc_jnienv()->ExceptionDescribe();
    xbmc_jnienv()->ExceptionClear();
  }
}

void CDVDMediaCodecInfo::RenderUpdate(const CRect &SrcRect, const CRect &DestRect)
{
  CSingleLock lock(m_section);

  static CRect cur_rect;

  if (!m_valid)
    return;

  if (DestRect != cur_rect)
  {
    CRect adjRect = CXBMCApp::MapRenderToDroid(DestRect);
    CXBMCApp::get()->setVideoViewSurfaceRect(adjRect.x1, adjRect.y1, adjRect.x2, adjRect.y2);
    CLog::Log(LOGDEBUG, "RenderUpdate: Dest - %f+%f-%fx%f", DestRect.x1, DestRect.y1, DestRect.Width(), DestRect.Height());
    CLog::Log(LOGDEBUG, "RenderUpdate: Adj  - %f+%f-%fx%f", adjRect.x1, adjRect.y1, adjRect.Width(), adjRect.Height());
    cur_rect = DestRect;

    // setVideoViewSurfaceRect is async, so skip rendering this frame
    ReleaseOutputBuffer(false);
  }
  else
    ReleaseOutputBuffer(true);
}


/*****************************************************************************/
/*****************************************************************************/
CDVDVideoCodecAndroidMediaCodec::CDVDVideoCodecAndroidMediaCodec(CProcessInfo &processInfo, bool surface_render)
: CDVDVideoCodec(processInfo)
, m_formatname("mediacodec")
, m_opened(false)
, m_checkForPicture(false)
, m_surface(nullptr)
, m_textureId(0)
, m_crypto(nullptr)
, m_bitstream(nullptr)
, m_render_sw(false)
, m_render_surface(surface_render)
, m_mpeg2_sequence(nullptr)
{
  memset(&m_videobuffer, 0x00, sizeof(VideoPicture));
}

CDVDVideoCodecAndroidMediaCodec::~CDVDVideoCodecAndroidMediaCodec()
{
  CLog::Log(LOGDEBUG, "CDVDMediaCodecInfo::Dtor");
  Dispose();

  if (m_crypto)
  {
    AMediaCrypto_delete(m_crypto);
    m_crypto = nullptr;
  }
  if (m_mpeg2_sequence)
  {
    delete (m_mpeg2_sequence);
    m_mpeg2_sequence = nullptr;
  }
}

std::atomic<bool> CDVDVideoCodecAndroidMediaCodec::m_InstanceGuard(false);

bool CDVDVideoCodecAndroidMediaCodec::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  int num_codecs;
  bool needSecureDecoder;

  m_opened = false;
  // allow only 1 instance here
  if (m_InstanceGuard.exchange(true))
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::Open - InstanceGuard locked\n");
    return false;
  }

  // mediacodec crashes with null size. Trap this...
  if (!hints.width || !hints.height)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::Open - %s\n", "null size, cannot handle");
    goto FAIL;
  }
  else if (hints.stills || hints.dvd)
  {
    // Won't work reliably
    goto FAIL;
  }
  else if (hints.orientation && m_render_surface && CJNIBase::GetSDKVersion() < 23)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::Open - %s\n", "Surface does not support orientation before API 23");
    goto FAIL;
  }
  else if (!CServiceBroker::GetSettings().GetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODEC) &&
           !CServiceBroker::GetSettings().GetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODECSURFACE))
    goto FAIL;

  m_render_surface = CServiceBroker::GetSettings().GetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODECSURFACE);
  m_drop = false;
  m_state = MEDIACODEC_STATE_UNINITIALIZED;
  m_noPictureLoop = 0;
  m_codecControlFlags = 0;
  m_hints = hints;
  m_indexInputBuffer = -1;

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: fpsrate %d / fpsscale %d\n", m_hints.fpsrate, m_hints.fpsscale);
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: CodecID %d \n", m_hints.codec);
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: StreamType %d \n", m_hints.type);
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: Level %d \n", m_hints.level);
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: Profile %d \n", m_hints.profile);
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: PTS_invalid %d \n", m_hints.ptsinvalid);
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: Tag %d \n", m_hints.codec_tag);
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open hints: %dx%d \n", m_hints.width,  m_hints.height);
  }

  switch(m_hints.codec)
  {
    case AV_CODEC_ID_MPEG2VIDEO:
      m_mime = "video/mpeg2";
      m_mpeg2_sequence = new mpeg2_sequence;
      m_mpeg2_sequence->width  = m_hints.width;
      m_mpeg2_sequence->height = m_hints.height;
      m_mpeg2_sequence->ratio  = m_hints.aspect;
      m_mpeg2_sequence->fps_scale = m_hints.fpsscale;
      m_mpeg2_sequence->fps_rate = m_hints.fpsrate;
      m_formatname = "amc-mpeg2";
      break;
    case AV_CODEC_ID_MPEG4:
      if (hints.width <= 800)
        goto FAIL;
      m_mime = "video/mp4v-es";
      m_formatname = "amc-mpeg4";
      break;
    case AV_CODEC_ID_H263:
      m_mime = "video/3gpp";
      m_formatname = "amc-h263";
      break;
    case AV_CODEC_ID_VP3:
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP8:
      //m_mime = "video/x-vp6";
      //m_mime = "video/x-vp7";
      m_mime = "video/x-vnd.on2.vp8";
      m_formatname = "amc-vpX";
      break;
    case AV_CODEC_ID_VP9:
      m_mime = "video/x-vnd.on2.vp9";
      m_formatname = "amc-vp9";
      break;
    case AV_CODEC_ID_AVS:
    case AV_CODEC_ID_CAVS:
    case AV_CODEC_ID_H264:
      switch(hints.profile)
      {
        case FF_PROFILE_H264_HIGH_10:
        case FF_PROFILE_H264_HIGH_10_INTRA:
          // No known h/w decoder supporting Hi10P
          goto FAIL;
      }
      m_mime = "video/avc";
      m_formatname = "amc-h264";
      // check for h264-avcC and convert to h264-annex-b
      if (m_hints.extradata && !m_hints.cryptoSession)
      {
        m_bitstream = new CBitstreamConverter;
        if (!m_bitstream->Open(m_hints.codec, (uint8_t*)m_hints.extradata, m_hints.extrasize, true))
        {
          SAFE_DELETE(m_bitstream);
        }
      }
      break;
    case AV_CODEC_ID_HEVC:
      m_mime = "video/hevc";
      m_formatname = "amc-h265";
      // check for hevc-hvcC and convert to h265-annex-b
      if (m_hints.extradata && !m_hints.cryptoSession)
      {
        m_bitstream = new CBitstreamConverter;
        if (!m_bitstream->Open(m_hints.codec, (uint8_t*)m_hints.extradata, m_hints.extrasize, true))
        {
          SAFE_DELETE(m_bitstream);
        }
      }
      break;
    case AV_CODEC_ID_WMV3:
      if (m_hints.extrasize == 4 || m_hints.extrasize == 5)
      {
        // Convert to SMPTE 421M-2006 Annex-L
        static char annexL_hdr1[] = {0x8e, 0x01, 0x00, 0xc5, 0x04, 0x00, 0x00, 0x00};
        static char annexL_hdr2[] = {0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        free(m_hints.extradata);
        m_hints.extrasize = 36;
        m_hints.extradata = malloc(m_hints.extrasize);

        unsigned int offset = 0;
        char buf[4];
        memcpy(m_hints.extradata, annexL_hdr1, sizeof(annexL_hdr1));
        offset += sizeof(annexL_hdr1);
        memcpy(&((char *)(m_hints.extradata))[offset], hints.extradata, 4);
        offset += 4;
        BS_WL32(buf, hints.height);
        memcpy(&((char *)(m_hints.extradata))[offset], buf, 4);
        offset += 4;
        BS_WL32(buf, hints.width);
        memcpy(&((char *)(m_hints.extradata))[offset], buf, 4);
        offset += 4;
        memcpy(&((char *)(m_hints.extradata))[offset], annexL_hdr2, sizeof(annexL_hdr2));
      }

      m_mime = "video/x-ms-wmv";
      m_formatname = "amc-wmv";
      break;
    case AV_CODEC_ID_VC1:
    {
      if (m_hints.extrasize < 16)
        goto FAIL;

      // Reduce extradata to first SEQ header
      unsigned int seq_offset = 0;
      for (; seq_offset <= m_hints.extrasize-4; ++seq_offset)
      {
        char *ptr = &((char*)m_hints.extradata)[seq_offset];
        if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x01 && ptr[3] == 0x0f)
          break;
      }
      if (seq_offset > m_hints.extrasize-4)
        goto FAIL;

      if (seq_offset)
      {
        free(m_hints.extradata);
        m_hints.extrasize -= seq_offset;
        m_hints.extradata = malloc(m_hints.extrasize);
        memcpy(m_hints.extradata, &((char *)(hints.extradata))[seq_offset], m_hints.extrasize);
      }

      m_mime = "video/wvc1";
      m_formatname = "amc-vc1";
      break;
    }
    default:
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open Unknown hints.codec(%d)", hints.codec);
      goto FAIL;
      break;
  }

  if (m_crypto)
  {
    AMediaCrypto_delete(m_crypto);
    m_crypto = nullptr;
  }

  if (m_hints.cryptoSession)
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open Initializing MediaCrypto");

    const AMediaUUID *uuid(nullptr);
    const AMediaUUID wvuuid = {0xED,0xEF,0x8B,0xA9,0x79,0xD6,0x4A,0xCE,0xA3,0xC8,0x27,0xDC,0xD5,0x1D,0x21,0xED};
    const AMediaUUID pruuid = {0x9A,0x04,0xF0,0x79,0x98,0x40,0x42,0x86,0xAB,0x92,0xE6,0x5B,0xE0,0x88,0x5F,0x95};

    if (m_hints.cryptoSession->keySystem == CRYPTO_SESSION_SYSTEM_WIDEVINE)
      uuid = &wvuuid;
    else if (m_hints.cryptoSession->keySystem == CRYPTO_SESSION_SYSTEM_PLAYREADY)
      uuid = &pruuid;
    else
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::Open Unsupported crypto-keysystem %u", m_hints.cryptoSession->keySystem);
      goto FAIL;
    }

    m_crypto = AMediaCrypto_new(*uuid, m_hints.cryptoSession->sessionId, m_hints.cryptoSession->sessionIdSize);

    if (xbmc_jnienv()->ExceptionCheck())
    {
      CLog::Log(LOGERROR, "MediaCrypto::ExceptionCheck: <init>");
      xbmc_jnienv()->ExceptionDescribe();
      xbmc_jnienv()->ExceptionClear();
      goto FAIL;
    }
  }

  if (m_render_surface)
  {
    m_jnivideosurface = CXBMCApp::get()->getVideoViewSurface();
    if (!m_jnivideosurface)
      goto FAIL;
    m_surface = ANativeWindow_fromSurface(xbmc_jnienv(), m_jnivideosurface.get_raw());
  }

  // CJNIMediaCodec::createDecoderByXXX doesn't handle errors nicely,
  // it crashes if the codec isn't found. This is fixed in latest AOSP,
  // but not in current 4.1 devices. So 1st search for a matching codec, then create it.
  m_codec = nullptr;
  m_colorFormat = -1;
  num_codecs = CJNIMediaCodecList::getCodecCount();
  needSecureDecoder = AMediaCrypto_requiresSecureDecoderComponent(m_mime.c_str());

  for (int i = 0; i < num_codecs; i++)
  {
    CJNIMediaCodecInfo codec_info = CJNIMediaCodecList::getCodecInfoAt(i);
    if (codec_info.isEncoder())
      continue;

    m_codecname = codec_info.getName();
    if (IsBlacklisted(m_codecname))
      continue;

    if (needSecureDecoder)
      m_codecname += ".secure";

    CJNIMediaCodecInfoCodecCapabilities codec_caps = codec_info.getCapabilitiesForType(m_mime);
    if (xbmc_jnienv()->ExceptionCheck())
    {
      // Unsupported type?
      xbmc_jnienv()->ExceptionClear();
      continue;
    }

    std::vector<int> color_formats = codec_caps.colorFormats();

    std::vector<std::string> types = codec_info.getSupportedTypes();
    // return the 1st one we find, that one is typically 'the best'
    for (size_t j = 0; j < types.size(); ++j)
    {
      if (types[j] == m_mime)
      {
        m_codec = AMediaCodec_createCodecByName(m_codecname.c_str());
        if (!m_codec)
        {
          CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::Open cannot create codec");
          m_codec = nullptr;
          continue;
        }

        for (size_t k = 0; k < color_formats.size(); ++k)
        {
          CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Open "
            "m_codecname(%s), colorFormat(%d)", m_codecname.c_str(), color_formats[k]);
          if (IsSupportedColorFormat(color_formats[k]))
            m_colorFormat = color_formats[k]; // Save color format for initial output configuration
        }
        break;
      }
    }
    if (m_codec)
      break;
  }
  if (!m_codec)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec:: Failed to create Android MediaCodec");
    goto FAIL;
  }

  // blacklist of devices that cannot surface render.
  m_render_sw = CanSurfaceRenderBlackList(m_codecname) || g_advancedSettings.m_mediacodecForceSoftwareRendering;
  if (m_render_sw)
  {
    if (m_colorFormat == -1)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec:: No supported color format");
      goto FAIL;
    }
    m_render_surface = false;
  }

  // setup a YUV420P VideoPicture buffer.
  // first make sure all properties are reset.
  memset(&m_videobuffer, 0x00, sizeof(VideoPicture));

  m_videobuffer.dts = DVD_NOPTS_VALUE;
  m_videobuffer.pts = DVD_NOPTS_VALUE;
  m_videobuffer.color_range  = 0;
  m_videobuffer.color_matrix = 4;
  m_videobuffer.iFlags  = DVP_FLAG_ALLOCATED;
  m_videobuffer.iWidth  = m_hints.width;
  m_videobuffer.iHeight = m_hints.height;
  // these will get reset to crop values later
  m_videobuffer.iDisplayWidth  = m_hints.width;
  m_videobuffer.iDisplayHeight = m_hints.height;

  if (!ConfigureMediaCodec())
    goto FAIL;

  CLog::Log(LOGINFO, "CDVDVideoCodecAndroidMediaCodec:: "
    "Open Android MediaCodec %s", m_codecname.c_str());

  m_opened = true;

  m_processInfo.SetVideoDecoderName(m_formatname, true );
  m_processInfo.SetVideoPixelFormat(m_render_surface ? "Surface" : (m_render_sw ? "YUV" : "EGL"));
  m_processInfo.SetVideoDimensions(m_hints.width, m_hints.height);
  m_processInfo.SetVideoDeintMethod("hardware");
  m_processInfo.SetVideoDAR(m_hints.aspect);

  return true;

FAIL:
  m_InstanceGuard.exchange(false);
  if (m_codec)
    m_codec = nullptr;
  SAFE_DELETE(m_bitstream);

  return false;
}

void CDVDVideoCodecAndroidMediaCodec::Dispose()
{
  if (!m_opened)
    return;

  // invalidate any inflight outputbuffers
  FlushInternal();

  // clear m_videobuffer bits
  if (m_render_sw)
  {
    free(m_videobuffer.data[0]), m_videobuffer.data[0] = NULL;
    free(m_videobuffer.data[1]), m_videobuffer.data[1] = NULL;
    free(m_videobuffer.data[2]), m_videobuffer.data[2] = NULL;
  }
  m_videobuffer.iFlags = 0;
  // m_videobuffer.mediacodec is unioned with m_videobuffer.data[0]
  // so be very careful when and how you touch it.
  m_videobuffer.hwPic = NULL;

  if (m_codec)
  {
    m_state = MEDIACODEC_STATE_UNINITIALIZED;
    AMediaCodec_stop(m_codec);
    AMediaCodec_delete(m_codec);
    m_codec = nullptr;
  }
  ReleaseSurfaceTexture();

  if(m_surface)
    ANativeWindow_release(m_surface);
  m_surface = nullptr;

  //if (m_render_surface)
  //  CXBMCApp::get()->clearVideoView();

  m_InstanceGuard.exchange(false);

  SAFE_DELETE(m_bitstream);

  m_opened = false;
}

bool CDVDVideoCodecAndroidMediaCodec::AddData(const DemuxPacket &packet)
{
  if (!m_opened)
    return false;

  double pts(packet.pts), dts(packet.dts);

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::AddData dts:%0.2lf pts:%0.2lf sz:%d indexBuffer:%d current state (%d)", dts, pts, packet.iSize, m_indexInputBuffer, m_state);
  else if (m_state != MEDIACODEC_STATE_RUNNING)
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::AddData current state (%d)", m_state);

  if (m_hints.ptsinvalid)
    pts = DVD_NOPTS_VALUE;

  uint8_t *pData(packet.pData);
  int iSize(packet.iSize);

  if (m_state == MEDIACODEC_STATE_ENDOFSTREAM)
  {
    // We received a packet but already reached EOS. Flush...
    FlushInternal();
    AMediaCodec_flush(m_codec);
    m_state = MEDIACODEC_STATE_FLUSHED;
  }

  if (pData && iSize)
  {
    if (m_indexInputBuffer >= 0)
    {
      if (m_state == MEDIACODEC_STATE_FLUSHED)
        m_state = MEDIACODEC_STATE_RUNNING;
      if (!(m_state == MEDIACODEC_STATE_FLUSHED || m_state == MEDIACODEC_STATE_RUNNING))
        CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::AddData: Wrong state (%d)", m_state);

      if (m_mpeg2_sequence && CBitstreamConverter::mpeg2_sequence_header(pData, iSize, m_mpeg2_sequence))
      {
        m_hints.fpsrate = m_mpeg2_sequence->fps_rate;
        m_hints.fpsscale = m_mpeg2_sequence->fps_scale;
        m_hints.width    = m_mpeg2_sequence->width;
        m_hints.height   = m_mpeg2_sequence->height;
        m_hints.aspect   = m_mpeg2_sequence->ratio;

        m_processInfo.SetVideoFps(static_cast<float>(m_hints.fpsrate) / m_hints.fpsscale);
        m_processInfo.SetVideoDAR(m_hints.aspect);
      }

      // we have an input buffer, fill it.
      if (pData && m_bitstream)
      {
        m_bitstream->Convert(pData, iSize);
        iSize = m_bitstream->GetConvertSize();
        pData = m_bitstream->GetConvertBuffer();
      }
      size_t out_size;
      uint8_t* dst_ptr = AMediaCodec_getInputBuffer(m_codec, m_indexInputBuffer, &out_size);
      if (iSize > out_size)
      {
        CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::Decode, iSize(%d) > size(%d)", iSize, out_size);
        iSize = out_size;
      }

      AMediaCodecCryptoInfo *cryptoInfo(0);
      if (m_crypto && packet.cryptoInfo)
      {
        std::vector<size_t> clearBytes(packet.cryptoInfo->clearBytes, packet.cryptoInfo->clearBytes + packet.cryptoInfo->numSubSamples);
        std::vector<size_t> cipherBytes(packet.cryptoInfo->cipherBytes, packet.cryptoInfo->cipherBytes + packet.cryptoInfo->numSubSamples);

        cryptoInfo = AMediaCodecCryptoInfo_new(
          packet.cryptoInfo->numSubSamples,
          packet.cryptoInfo->kid,
          packet.cryptoInfo->iv,
          AMEDIACODECRYPTOINFO_MODE_AES_CTR,
          &clearBytes[0], &cipherBytes[0]);
      }
      if (dst_ptr)
      {
        // Codec specifics
        switch(m_hints.codec)
        {
          case AV_CODEC_ID_VC1:
          {
            if (iSize >= 4 && pData[0] == 0x00 && pData[1] == 0x00 && pData[2] == 0x01 && (pData[3] == 0x0d || pData[3] == 0x0f))
              memcpy(dst_ptr, pData, iSize);
            else
            {
              dst_ptr[0] = 0x00;
              dst_ptr[1] = 0x00;
              dst_ptr[2] = 0x01;
              dst_ptr[3] = 0x0d;
              memcpy(dst_ptr+4, pData, iSize);
              iSize += 4;
            }

            break;
          }

          default:
            memcpy(dst_ptr, pData, iSize);
            break;
        }
      }

      // Translate from VideoPlayer dts/pts to MediaCodec pts,
      // pts WILL get re-ordered by MediaCodec if needed.
      // Do not try to pass pts as a unioned double/int64_t,
      // some android devices will diddle with presentationTimeUs
      // and you will get NaN back and VideoPlayerVideo will barf.
      int64_t presentationTimeUs = 0;
      if (pts != DVD_NOPTS_VALUE)
        presentationTimeUs = pts;
      else if (dts != DVD_NOPTS_VALUE)
        presentationTimeUs = dts;

      int flags = 0;
      int offset = 0;

      media_status_t mstat;
      if (!cryptoInfo)
        mstat = AMediaCodec_queueInputBuffer(m_codec, m_indexInputBuffer, offset, iSize, presentationTimeUs, flags);
      else
      {
        mstat = AMediaCodec_queueSecureInputBuffer(m_codec, m_indexInputBuffer, offset, cryptoInfo, presentationTimeUs, flags);
        AMediaCodecCryptoInfo_delete(cryptoInfo);
      }
      m_indexInputBuffer = -1;
      if (mstat != AMEDIA_OK)
        CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::AddData error(%d)", mstat);
    }
    else
      return false;
  }
  return true;
}

void CDVDVideoCodecAndroidMediaCodec::Reset()
{
  if (!m_opened)
    return;

  if (m_codec)
  {
    // flush all outputbuffers inflight, they will
    // become invalid on m_codec->flush and generate
    // a spew of java exceptions if used
    FlushInternal();

    // now we can flush the actual MediaCodec object
    CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Reset Current state (%d)", m_state);
    m_state = MEDIACODEC_STATE_FLUSHED;
    AMediaCodec_flush(m_codec);

    // Invalidate our local VideoPicture bits
    m_videobuffer.pts = DVD_NOPTS_VALUE;
    if (!m_render_sw)
      m_videobuffer.hwPic = NULL;

    m_indexInputBuffer = -1;
    m_checkForPicture = false;
  }
}

bool CDVDVideoCodecAndroidMediaCodec::Reconfigure(CDVDStreamInfo &hints)
{
  CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::Reconfigure called");
  return false;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecAndroidMediaCodec::GetPicture(VideoPicture* pVideoPicture)
{
  if (!m_opened)
    return VC_NONE;

  if (m_checkForPicture)
  {
    int retgp = GetOutputPicture();
    if (retgp > 0)
    {
      m_checkForPicture = false;
      m_noPictureLoop = 0;
      *pVideoPicture = m_videobuffer;

      // Invalidate our local VideoPicture bits
      m_videobuffer.pts = DVD_NOPTS_VALUE;
      if (!m_render_sw)
        m_videobuffer.hwPic = NULL;

      if (g_advancedSettings.CanLogComponent(LOGVIDEO))
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::GetPicture pts:%0.4lf", pVideoPicture->pts);

      return VC_PICTURE;
    }
    else if (retgp == -1 || ((m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)!=0 && ++m_noPictureLoop == 10))  // EOS
    {
      m_state = MEDIACODEC_STATE_ENDOFSTREAM;
      m_noPictureLoop = 0;
      return VC_NONE;
    }
  }

  m_checkForPicture = true;

  // try to fetch an input buffer
  if (m_indexInputBuffer < 0)
    m_indexInputBuffer = AMediaCodec_dequeueInputBuffer(m_codec, 5000 /*timout*/);

  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::Decode ExceptionCheck");
    xbmc_jnienv()->ExceptionDescribe();
    xbmc_jnienv()->ExceptionClear();
    return VC_ERROR;
  }
  else if (m_indexInputBuffer >= 0)
  {
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec::GetPicture VC_BUFFER");
    return VC_BUFFER;
  }

  return VC_NONE;
}

bool CDVDVideoCodecAndroidMediaCodec::ClearPicture(VideoPicture* pVideoPicture)
{
  if (pVideoPicture->hwPic && (pVideoPicture->format == RENDER_FMT_MEDIACODEC || pVideoPicture->format == RENDER_FMT_MEDIACODECSURFACE))
    static_cast<CDVDMediaCodecInfo*>(pVideoPicture->hwPic)->Release();
  memset(pVideoPicture, 0x00, sizeof(VideoPicture));

  return true;
}

void CDVDVideoCodecAndroidMediaCodec::SetCodecControl(int flags)
{
  if (m_codecControlFlags != flags)
  {
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "%s %x->%x",  __func__, m_codecControlFlags, flags);
    m_codecControlFlags = flags;

    m_drop = (flags & DVD_CODEC_CTRL_DROP) != 0;

    if (m_drop)
      m_videobuffer.iFlags |= DVP_FLAG_DROPPED;
    else
      m_videobuffer.iFlags &= ~DVP_FLAG_DROPPED;
  }
}

unsigned CDVDVideoCodecAndroidMediaCodec::GetAllowedReferences()
{
  return 4;
}

void CDVDVideoCodecAndroidMediaCodec::FlushInternal()
{
  // invalidate any existing inflight buffers and create
  // new ones to match the number of output buffers

  if (m_render_sw)
    return;

  for (size_t i = 0; i < m_inflight.size(); i++)
  {
    m_inflight[i]->Validate(false);
    m_inflight[i]->Release();
  }
  m_inflight.clear();
}

bool CDVDVideoCodecAndroidMediaCodec::ConfigureMediaCodec(void)
{
  // setup a MediaFormat to match the video content,
  // used by codec during configure
  AMediaFormat* mediaformat = AMediaFormat_new();
  AMediaFormat_setString(mediaformat, AMEDIAFORMAT_KEY_MIME, m_mime.c_str());
  AMediaFormat_setInt32(mediaformat, AMEDIAFORMAT_KEY_WIDTH, m_hints.width);
  AMediaFormat_setInt32(mediaformat, AMEDIAFORMAT_KEY_HEIGHT, m_hints.height);
  AMediaFormat_setInt32(mediaformat, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 0);

  if (CJNIBase::GetSDKVersion() >= 23 && m_render_surface)
  {
    // Handle rotation
    AMediaFormat_setInt32(mediaformat, XMEDIAFORMAT_KEY_ROTATION, m_hints.orientation);
  }


  // handle codec extradata
  if (m_hints.extrasize)
  {
    size_t size = m_hints.extrasize;
    void  *src_ptr = m_hints.extradata;
    if (m_bitstream)
    {
      size = m_bitstream->GetExtraSize();
      src_ptr = m_bitstream->GetExtraData();
    }

    AMediaFormat_setBuffer(mediaformat, "csd-0", src_ptr, size);
  }

  if (!m_render_sw && !m_render_surface)
    InitSurfaceTexture();

  // configure and start the codec.
  // use the MediaFormat that we have setup.
  // use a null MediaCrypto, our content is not encrypted.

  int flags = 0;
  media_status_t mstat;
  if (m_render_sw)
    mstat = AMediaCodec_configure(m_codec, mediaformat, nullptr, m_crypto, flags);
  else
    mstat = AMediaCodec_configure(m_codec, mediaformat, m_surface, m_crypto, flags);

  if (mstat != AMEDIA_OK)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec configure error: %d", mstat);
    return false;
  }
  m_state = MEDIACODEC_STATE_CONFIGURED;

  mstat = AMediaCodec_start(m_codec);
  if (mstat != AMEDIA_OK)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec start error: %d", mstat);
    return false;
  }
  m_state = MEDIACODEC_STATE_FLUSHED;

  // There is no guarantee we'll get an INFO_OUTPUT_FORMAT_CHANGED (up to Android 4.3)
  // Configure the output with defaults
  ConfigureOutputFormat(mediaformat);

  return true;
}

int CDVDVideoCodecAndroidMediaCodec::GetOutputPicture(void)
{
  int rtn = 0;

  int64_t timeout_us = 10000;
  AMediaCodecBufferInfo bufferInfo;
  ssize_t index = AMediaCodec_dequeueOutputBuffer(m_codec, &bufferInfo, timeout_us);
  if (index >= 0)
  {
    int64_t pts = bufferInfo.presentationTimeUs;
    m_videobuffer.dts = DVD_NOPTS_VALUE;
    m_videobuffer.pts = DVD_NOPTS_VALUE;
    if (pts != AV_NOPTS_VALUE)
      m_videobuffer.pts = pts;

    if (m_drop)
    {
      AMediaCodec_releaseOutputBuffer(m_codec, index, false);
      m_videobuffer.hwPic = nullptr;
      return 1;
    }

    int flags = bufferInfo.flags;
    if (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: BUFFER_FLAG_END_OF_STREAM");
      AMediaCodec_releaseOutputBuffer(m_codec, index, false);
      return -1;
    }

    if (!m_render_sw)
    {
      size_t i = 0;
      for (; i < m_inflight.size(); ++i)
      {
        if (m_inflight[i]->GetIndex() == index)
          break;
      }
      if (i == m_inflight.size())
        m_inflight.push_back(
          new CDVDMediaCodecInfo(index, m_textureId, m_codec, m_surfaceTexture, m_frameAvailable)
        );
      m_videobuffer.hwPic = m_inflight[i]->Retain();
      static_cast<CDVDMediaCodecInfo*>(m_videobuffer.hwPic)->Validate(true);
    }
    else
    {
      size_t out_size;
      uint8_t* buffer = AMediaCodec_getOutputBuffer(m_codec, index, &out_size);
      if (buffer && out_size)
      {
        int loop_end = 0;
        if (m_videobuffer.format == RENDER_FMT_NV12)
          loop_end = 2;
        else if (m_videobuffer.format == RENDER_FMT_YUV420P)
          loop_end = 3;

        for (int i = 0; i < loop_end; i++)
        {
          uint8_t *src   = buffer + m_src_offset[i];
          int src_stride = m_src_stride[i];
          uint8_t *dst   = m_videobuffer.data[i];
          int dst_stride = m_videobuffer.iLineSize[i];

          int height = m_videobuffer.iHeight;
          if (i > 0)
            height = (m_videobuffer.iHeight + 1) / 2;

          if (src_stride == dst_stride)
            memcpy(dst, src, dst_stride * height);
          else
            for (int j = 0; j < height; j++, src += src_stride, dst += dst_stride)
              memcpy(dst, src, dst_stride);
        }
      }
      media_status_t mstat = AMediaCodec_releaseOutputBuffer(m_codec, index, false);
      if (mstat != AMEDIA_OK)
        CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::GetOutputPicture error: releaseOutputBuffer(%d)", mstat);
    }
    rtn = 1;
  }
  else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
  {
    AMediaFormat* mediaformat = AMediaCodec_getOutputFormat(m_codec);
    if (!mediaformat)
      CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::GetOutputPicture(INFO_OUTPUT_FORMAT_CHANGED) ExceptionCheck: getOutputBuffers");
    else
      ConfigureOutputFormat(mediaformat);
  }
  else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
  {
    // ignore
    rtn = 0;
  }
  else
  {
    // we should never get here
    CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec::GetOutputPicture unknown index(%d)", index);
    rtn = -2;
  }

  return rtn;
}

void CDVDVideoCodecAndroidMediaCodec::ConfigureOutputFormat(AMediaFormat* mediaformat)
{
  int width       = 0;
  int height      = 0;
  int stride      = 0;
  int slice_height= 0;
  int color_format= 0;
  int crop_left   = 0;
  int crop_top    = 0;
  int crop_right  = 0;
  int crop_bottom = 0;

  int tmpVal;
  if (AMediaFormat_getInt32(mediaformat, AMEDIAFORMAT_KEY_WIDTH, &tmpVal))
    width = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, AMEDIAFORMAT_KEY_HEIGHT, &tmpVal))
    height = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, AMEDIAFORMAT_KEY_STRIDE, &tmpVal))
    stride = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, XMEDIAFORMAT_KEY_SLICE, &tmpVal))
    slice_height = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, AMEDIAFORMAT_KEY_COLOR_FORMAT, &tmpVal))
    color_format = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, XMEDIAFORMAT_KEY_CROP_LEFT, &tmpVal))
    crop_left = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, XMEDIAFORMAT_KEY_CROP_RIGHT, &tmpVal))
    crop_right = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, XMEDIAFORMAT_KEY_CROP_TOP, &tmpVal))
    crop_top = tmpVal;
  if (AMediaFormat_getInt32(mediaformat, XMEDIAFORMAT_KEY_CROP_BOTTOM, &tmpVal))
    crop_bottom = tmpVal;

  if (!crop_right)
    crop_right = width-1;
  if (!crop_bottom)
    crop_bottom = height-1;

  // clear any jni exceptions
  if (xbmc_jnienv()->ExceptionCheck())
    xbmc_jnienv()->ExceptionClear();

  CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: "
    "width(%d), height(%d), stride(%d), slice-height(%d), color-format(%d)",
    width, height, stride, slice_height, color_format);
  CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: "
    "crop-left(%d), crop-top(%d), crop-right(%d), crop-bottom(%d)",
    crop_left, crop_top, crop_right, crop_bottom);

  if (!m_render_sw)
  {
    if (m_render_surface)
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: Multi-Surface Rendering");
      m_videobuffer.format = RENDER_FMT_MEDIACODECSURFACE;
    }
    else
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: Direct Surface Rendering");
      m_videobuffer.format = RENDER_FMT_MEDIACODEC;
    }
  }
  else
  {
    // Android device quirks and fixes

    // Samsung Quirk: ignore width/height/stride/slice: http://code.google.com/p/android/issues/detail?id=37768#c3
    if (strstr(m_codecname.c_str(), "OMX.SEC.avc.dec") != NULL || strstr(m_codecname.c_str(), "OMX.SEC.avcdec") != NULL)
    {
      width = stride = m_hints.width;
      height = slice_height = m_hints.height;
    }
    // No color-format? Initialize with the one we detected as valid earlier
    if (color_format == 0)
      color_format = m_colorFormat;
    if (stride <= width)
      stride = width;
    if (slice_height <= height)
    {
      slice_height = height;
      if (color_format == CJNIMediaCodecInfoCodecCapabilities::COLOR_FormatYUV420Planar)
      {
        // NVidia Tegra 3 on Nexus 7 does not set slice_heights
        if (strstr(m_codecname.c_str(), "OMX.Nvidia.") != NULL)
        {
          slice_height = (((height) + 15) & ~15);
          CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: NVidia Tegra 3 quirk, slice_height(%d)", slice_height);
        }
      }
    }
    if (color_format == CJNIMediaCodecInfoCodecCapabilities::COLOR_TI_FormatYUV420PackedSemiPlanar)
    {
      slice_height -= crop_top / 2;
      // set crop top/left here, since the offset parameter already includes this.
      // if we would ignore the offset parameter in the BufferInfo, we could just keep
      // the original slice height and apply the top/left cropping instead.
      crop_top = 0;
      crop_left = 0;
    }

    // default picture format to none
    for (int i = 0; i < 4; i++)
      m_src_offset[i] = m_src_stride[i] = 0;
    // delete any existing buffers
    for (int i = 0; i < 4; i++)
      free(m_videobuffer.data[i]);

    // setup picture format and data offset vectors
    if (color_format == CJNIMediaCodecInfoCodecCapabilities::COLOR_FormatYUV420Planar)
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: COLOR_FormatYUV420Planar");

      // Y plane
      m_src_stride[0] = stride;
      m_src_offset[0] = crop_top * stride;
      m_src_offset[0]+= crop_left;

      // U plane
      m_src_stride[1] = (stride + 1) / 2;
      //  skip over the Y plane
      m_src_offset[1] = slice_height * stride;
      //  crop_top/crop_left divided by two
      //  because one byte of the U/V planes
      //  corresponds to two pixels horizontally/vertically
      m_src_offset[1]+= crop_top  / 2 * m_src_stride[1];
      m_src_offset[1]+= crop_left / 2;

      // V plane
      m_src_stride[2] = (stride + 1) / 2;
      //  skip over the Y plane
      m_src_offset[2] = slice_height * stride;
      //  skip over the U plane
      m_src_offset[2]+= ((slice_height + 1) / 2) * ((stride + 1) / 2);
      //  crop_top/crop_left divided by two
      //  because one byte of the U/V planes
      //  corresponds to two pixels horizontally/vertically
      m_src_offset[2]+= crop_top  / 2 * m_src_stride[2];
      m_src_offset[2]+= crop_left / 2;

      m_videobuffer.iLineSize[0] =  width;         // Y
      m_videobuffer.iLineSize[1] = (width + 1) /2; // U
      m_videobuffer.iLineSize[2] = (width + 1) /2; // V
      m_videobuffer.iLineSize[3] = 0;

      unsigned int iPixels = width * height;
      unsigned int iChromaPixels = iPixels/4;
      m_videobuffer.data[0] = (uint8_t*)malloc(16 + iPixels);
      m_videobuffer.data[1] = (uint8_t*)malloc(16 + iChromaPixels);
      m_videobuffer.data[2] = (uint8_t*)malloc(16 + iChromaPixels);
      m_videobuffer.data[3] = NULL;
      m_videobuffer.format  = RENDER_FMT_YUV420P;
    }
    else if (color_format == CJNIMediaCodecInfoCodecCapabilities::COLOR_FormatYUV420SemiPlanar
          || color_format == CJNIMediaCodecInfoCodecCapabilities::COLOR_QCOM_FormatYUV420SemiPlanar
          || color_format == CJNIMediaCodecInfoCodecCapabilities::COLOR_TI_FormatYUV420PackedSemiPlanar
          || color_format == CJNIMediaCodecInfoCodecCapabilities::OMX_QCOM_COLOR_FormatYVU420SemiPlanarInterlace)

    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecAndroidMediaCodec:: COLOR_FormatYUV420SemiPlanar");

      // Y plane
      m_src_stride[0] = stride;
      m_src_offset[0] = crop_top * stride;
      m_src_offset[0]+= crop_left;

      // UV plane
      m_src_stride[1] = stride;
      //  skip over the Y plane
      m_src_offset[1] = slice_height * stride;
      m_src_offset[1]+= crop_top * stride;
      m_src_offset[1]+= crop_left;

      m_videobuffer.iLineSize[0] = width;  // Y
      m_videobuffer.iLineSize[1] = width;  // UV
      m_videobuffer.iLineSize[2] = 0;
      m_videobuffer.iLineSize[3] = 0;

      unsigned int iPixels = width * height;
      unsigned int iChromaPixels = iPixels;
      m_videobuffer.data[0] = (uint8_t*)malloc(16 + iPixels);
      m_videobuffer.data[1] = (uint8_t*)malloc(16 + iChromaPixels);
      m_videobuffer.data[2] = NULL;
      m_videobuffer.data[3] = NULL;
      m_videobuffer.format  = RENDER_FMT_NV12;
    }
    else
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecAndroidMediaCodec:: Fixme unknown color_format(%d)", color_format);
      return;
    }
  }

  if (crop_right)
    width = crop_right  + 1 - crop_left;
  if (crop_bottom)
    height = crop_bottom + 1 - crop_top;

  m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth  = width;
  m_videobuffer.iDisplayHeight = m_videobuffer.iHeight = height;

  if (m_hints.aspect > 1.0 && !m_hints.forced_aspect)
  {
    m_videobuffer.iDisplayWidth  = ((int)lrint(m_videobuffer.iHeight * m_hints.aspect)) & ~3;
    if (m_videobuffer.iDisplayWidth > m_videobuffer.iWidth)
    {
      m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
      m_videobuffer.iDisplayHeight = ((int)lrint(m_videobuffer.iWidth / m_hints.aspect)) & ~3;
    }
  }
}

void CDVDVideoCodecAndroidMediaCodec::CallbackInitSurfaceTexture(void *userdata)
{
  CDVDVideoCodecAndroidMediaCodec *ctx = static_cast<CDVDVideoCodecAndroidMediaCodec*>(userdata);
  ctx->InitSurfaceTexture();
}

void CDVDVideoCodecAndroidMediaCodec::InitSurfaceTexture(void)
{
  if (m_render_sw || m_render_surface)
    return;

  // We MUST create the GLES texture on the main thread
  // to match where the valid GLES context is located.
  // It would be nice to move this out of here, we would need
  // to create/fetch/create from g_RenderManager. But g_RenderManager
  // does not know we are using MediaCodec until Configure and we
  // we need m_surfaceTexture valid before then. Chicken, meet Egg.
  if (g_application.IsCurrentThread())
  {
    // localize GLuint so we do not spew gles includes in our header
    GLuint texture_id;

    glGenTextures(1, &texture_id);
    glBindTexture(  GL_TEXTURE_EXTERNAL_OES, texture_id);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(  GL_TEXTURE_EXTERNAL_OES, 0);
    m_textureId = texture_id;

    m_surfaceTexture = std::shared_ptr<CJNISurfaceTexture>(new CJNISurfaceTexture(m_textureId));
    // hook the surfaceTexture OnFrameAvailable callback
    m_frameAvailable = std::shared_ptr<CDVDMediaCodecOnFrameAvailable>(new CDVDMediaCodecOnFrameAvailable(m_surfaceTexture));
    m_jnisurface = new CJNISurface(*m_surfaceTexture);
    m_surface = ANativeWindow_fromSurface(xbmc_jnienv(), m_jnisurface->get_raw());
  }
  else
  {
    ThreadMessageCallback callbackData;
    callbackData.callback = &CallbackInitSurfaceTexture;
    callbackData.userptr  = (void*)this;

    // wait for it.
    CApplicationMessenger::GetInstance().SendMsg(TMSG_CALLBACK, -1, -1, static_cast<void*>(&callbackData));
  }

  return;
}

void CDVDVideoCodecAndroidMediaCodec::ReleaseSurfaceTexture(void)
{
  if (m_render_sw || m_render_surface)
    return;

  // it is safe to delete here even though these items
  // were created in the main thread instance
  SAFE_DELETE(m_jnisurface);
  m_frameAvailable.reset();
  m_surfaceTexture.reset();

  if (m_textureId > 0)
  {
    GLuint texture_id = m_textureId;
    glDeleteTextures(1, &texture_id);
    m_textureId = 0;
  }
}
