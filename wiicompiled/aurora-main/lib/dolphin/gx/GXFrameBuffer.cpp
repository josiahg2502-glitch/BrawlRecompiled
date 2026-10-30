extern "C" int g_spmBBoxArmed;
namespace aurora::gx::fifo { extern float g_spmScreenWiden; }
#include <atomic>
#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/tex_copy_conv.hpp"
#include "../../gfx/efb_ram_copy.hpp"
#include "../../gfx/texture.hpp"
#include "../../gx/fifo.hpp"
#include "../../internal.hpp"
#include "../../window.hpp"
#include "../../gfx/clear.hpp"
#include "../../webgpu/gpu.hpp"
#include "../vi/vi_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
struct CopyClearState {
  bool clearColor = false;
  bool clearAlpha = false;
  bool clearDepth = false;
  aurora::Vec4<float> clearColorValue{0.f, 0.f, 0.f, 1.f};
};

CopyClearState get_copy_clear_state(GXBool clear) {
  if (!clear) {
    return {};
  }

  CopyClearState state{
      .clearColor = g_gxState.colorUpdate,
      .clearAlpha = g_gxState.alphaUpdate,
      .clearDepth = g_gxState.depthUpdate,
      .clearColorValue = g_gxState.clearColor,
  };

  if (!aurora::gx::render_target_has_alpha(g_gxState.pixelFmt)) {
    // RGB and depth EFB formats do not have an alpha channel to clear.
    state.clearAlpha = false;
  }

  return state;
}

struct CopySourceRect {
  aurora::gfx::ClipRect clearRect;
  aurora::Vec4<float> sampleRect;
};

CopySourceRect map_texture_copy_source(const aurora::gfx::ClipRect& source, bool renderSpace) {
  if (renderSpace || g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {
        .clearRect = source,
        .sampleRect = {static_cast<float>(source.x), static_cast<float>(source.y), static_cast<float>(source.width),
                       static_cast<float>(source.height)},
    };
  }

  const auto [logicalWidth, logicalHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalWidth == 0 || logicalHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {
        .clearRect = source,
        .sampleRect = {static_cast<float>(source.x), static_cast<float>(source.y), static_cast<float>(source.width),
                       static_cast<float>(source.height)},
    };
  }

  struct MappedEdge {
    float sample;
    int32_t nearest;
  };
  const auto mapEdge = [](int32_t edge, uint32_t logicalSize, uint32_t targetSize) {
    // Round scaled copy edges consistently to avoid seams and off-by-one pixels.
    const int64_t numerator = static_cast<int64_t>(edge) * static_cast<int64_t>(targetSize);
    const int64_t denominator = static_cast<int64_t>(logicalSize);
    const int64_t quotient = numerator / denominator;
    const int64_t remainder = numerator % denominator;
    const int64_t remainderMagnitude = remainder < 0 ? -remainder : remainder;
    const int64_t roundingThreshold = (denominator + 1) / 2;
    const int64_t nearestValue =
        quotient + (remainderMagnitude >= roundingThreshold ? (numerator < 0 ? -1 : 1) : 0);
    return MappedEdge{
        .sample = static_cast<float>(static_cast<double>(numerator) / static_cast<double>(denominator)),
        .nearest = static_cast<int32_t>(nearestValue),
    };
  };
  const auto left = mapEdge(source.x, logicalWidth, targetWidth);
  const auto top = mapEdge(source.y, logicalHeight, targetHeight);
  const auto right = mapEdge(source.x + source.width, logicalWidth, targetWidth);
  const auto bottom = mapEdge(source.y + source.height, logicalHeight, targetHeight);
  return {
      .clearRect =
          {
              .x = left.nearest,
              .y = top.nearest,
              .width = std::max<int32_t>(right.nearest - left.nearest, 1),
              .height = std::max<int32_t>(bottom.nearest - top.nearest, 1),
          },
      .sampleRect = {left.sample, top.sample, right.sample - left.sample, bottom.sample - top.sample},
  };
}





u32 pack_copy_filter_samples(u8 reg, const std::array<std::array<u8, 2>, 12>& samplePattern, size_t first) {
  u32 value = static_cast<u32>(reg) << 24;
  for (size_t i = 0; i < 6; ++i) {
    const size_t sample = first + i;
    const u8 component = samplePattern[sample / 2][sample % 2] & 0x0fu;
    value |= static_cast<u32>(component) << (i * 4);
  }
  return value;
}

u32 pack_copy_filter0(const std::array<u8, 7>& vfilter) {
  return 0x53000000u | (static_cast<u32>(vfilter[0] & 0x3fu) << 0) |
         (static_cast<u32>(vfilter[1] & 0x3fu) << 6) | (static_cast<u32>(vfilter[2] & 0x3fu) << 12) |
         (static_cast<u32>(vfilter[3] & 0x3fu) << 18);
}

u32 pack_copy_filter1(const std::array<u8, 7>& vfilter) {
  return 0x54000000u | (static_cast<u32>(vfilter[4] & 0x3fu) << 0) |
         (static_cast<u32>(vfilter[5] & 0x3fu) << 6) | (static_cast<u32>(vfilter[6] & 0x3fu) << 12);
}

std::array<u32, 3> combined_copy_filter_coefficients(const std::array<u8, 7>& vfilter) {
  if (!g_gxState.copyFilterVf) {
    return {0, 64, 0};
  }

  return {
      static_cast<u32>(vfilter[0]) + static_cast<u32>(vfilter[1]),
      static_cast<u32>(vfilter[2]) + static_cast<u32>(vfilter[3]) + static_cast<u32>(vfilter[4]),
      static_cast<u32>(vfilter[5]) + static_cast<u32>(vfilter[6]),
  };
}

aurora::Vec2<uint32_t> scale_copy_dst(u32 logicalWidth, u32 logicalHeight) {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {logicalWidth, logicalHeight};
  }

  const auto [logicalFbWidth, logicalFbHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {logicalWidth, logicalHeight};
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const auto scaledWidth = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalWidth) * scaleX)), 1);
  const auto scaledHeight = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalHeight) * scaleY)), 1);
  return {scaledWidth, scaledHeight};
}

} // namespace
// NSMBW profiling (AuroraTakeGxStats): texture copies, and those that needed a new GPU texture.
std::atomic<uint64_t> g_statCopies{0}, g_statCopyCreates{0};
namespace {
aurora::gfx::TextureHandle create_copy_texture(u32 width, u32 height, GXTexFmt texCopyFmt) {
  g_statCopyCreates.fetch_add(1, std::memory_order_relaxed);
  if (aurora::gfx::tex_copy_conv::needs_conversion(texCopyFmt)) {
    return aurora::gfx::new_conv_texture(width, height, texCopyFmt, "Copy Conv Texture");
  }

  const auto fmt = texCopyFmt == GX_TF_RGB565 ? GX_TF_RGB565 : GX_TF_RGBA8;
  return aurora::gfx::new_render_texture(width, height, fmt, "Resolved Texture");
}

  // Reuse retired copy targets to avoid unbounded GPU texture allocation.
struct CopyTexturePoolEntry {
  aurora::gx::GXState::CopyTextureKey key;
  u32 scaledWidth = 0;
  u32 scaledHeight = 0;
  aurora::gfx::TextureHandle handle;
  u32 lastUsedFrame = 0;
};
std::vector<CopyTexturePoolEntry> g_copyTexturePool;
// Reusable copy targets per destination. NSMBW copies the EFB to the same destination many times a
// frame in some courses (World 5's tower: 40 copies a frame), and each copy sampled earlier in the
// frame, or still held by a frame in flight, needs a target of its own. With only 3 kept, 18 GPU
// textures were created and destroyed every frame there (the slowdown). Targets unused for a few
// seconds are released instead.
constexpr size_t kCopyTexturePoolPerKey = 64;
constexpr u32 kCopyTextureIdleFrames = 300;

aurora::gfx::TextureHandle acquire_copy_texture(const aurora::gx::GXState::CopyTextureKey& key, u32 width, u32 height,
                                                GXTexFmt texCopyFmt) {
  size_t sameKey = 0;
  const u32 frame = aurora::gfx::current_frame();
  for (auto it = g_copyTexturePool.begin(); it != g_copyTexturePool.end();) {
    if (!(it->key == key)) {
      if (frame - it->lastUsedFrame > kCopyTextureIdleFrames && it->handle.use_count() <= 1) {
        it = g_copyTexturePool.erase(it);  // idle for seconds: give the memory back
        continue;
      }
      ++it;
      continue;
    }
    // Discard retired targets when their internal resolution changes.
    if ((it->scaledWidth != width || it->scaledHeight != height) && it->handle.use_count() <= 1) {
      it = g_copyTexturePool.erase(it);
      continue;
    }
    ++sameKey;
    if (it->scaledWidth == width && it->scaledHeight == height && it->handle.use_count() == 1) {
      it->lastUsedFrame = frame;
      return it->handle;
    }
    ++it;
  }

  auto handle = create_copy_texture(width, height, texCopyFmt);
  if (sameKey < kCopyTexturePoolPerKey) {
    g_copyTexturePool.push_back({key, width, height, handle, frame});
  }
  return handle;
}

u16 get_num_xfb_lines_internal(u16 efbHeight, u32 iScale) {
  CHECK(efbHeight != 0, "GXGetNumXfbLines requires non-zero EFB height");
  CHECK(iScale != 0, "invalid XFB line scale");

  const u32 count = static_cast<u32>(efbHeight - 1u) * 0x100u;
  u32 realHeight = (count / iScale) + 1u;

  u32 reducedScale = iScale;
  if (reducedScale > 0x80u && reducedScale < 0x100u) {
    while ((reducedScale & 1u) == 0u) {
      reducedScale >>= 1;
    }
    if (reducedScale != 0u && (efbHeight % reducedScale) == 0u) {
      ++realHeight;
    }
  }

  return static_cast<u16>(std::min<u32>(realHeight, 0x400u));
}

u32 y_scale_to_integer(f32 yScale) {
  CHECK(yScale > 0.f, "GX display copy y-scale must be positive");
  return static_cast<u32>(256.f / yScale) & 0x1ffu;
}

} // namespace

namespace aurora::gx {
void prune_copy_texture_pool(const void* dest) noexcept {
  if (dest == nullptr) {
    g_copyTexturePool.clear();
    return;
  }
  std::erase_if(g_copyTexturePool, [dest](const CopyTexturePoolEntry& entry) { return entry.key.dest == dest; });
}
} // namespace aurora::gx

extern "C" {
GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {0, 0, 21, 22, 21, 0, 0},
};
GXRenderModeObj GXPal528IntDf = {
    VI_TVMODE_PAL_INT,
    704,
    528,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXMpal480IntDf = {
    VI_TVMODE_PAL_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};

void GXAdjustForOverscan(GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
  *rmout = *rmin;
  const auto renderSize = aurora::gfx::get_render_target_size();
  rmout->fbWidth = static_cast<u16>(std::min<uint32_t>(renderSize.x, UINT16_MAX));
  rmout->efbHeight = static_cast<u16>(std::min<uint32_t>(renderSize.y, UINT16_MAX));
  rmout->xfbHeight = static_cast<u16>(std::min<uint32_t>(renderSize.y, UINT16_MAX));
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  g_gxState.dispCopySrc = {left, top, wd, ht};
  GX_WRITE_RAS_REG(0x49000000u | ((static_cast<u32>(top) & 0x3ffu) << 10) | (static_cast<u32>(left) & 0x3ffu));
  GX_WRITE_RAS_REG(0x4a000000u | (((static_cast<u32>(ht) - 1u) * 0x400u) & 0x000ffc00u) |
                   ((static_cast<u32>(wd) - 1u) & 0x3ffu));
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  g_gxState.texCopySrc = {left, top, wd, ht};
  g_gxState.texCopySrcRenderSpace = false;
}

void GXSetDispCopyDst(u16 wd, u16 ht) {
  g_gxState.dispCopyDstWidth = wd;
  g_gxState.dispCopyDstHeight = ht;
  GX_WRITE_RAS_REG(0x4d000000u | ((((static_cast<u32>(wd) & 0x7fffu) << 1) >> 5) & 0x3ffu));
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
  g_gxState.texCopyFmt = fmt;
  g_gxState.texCopyDstWidth = wd;
  g_gxState.texCopyDstHeight = ht;
  g_gxState.texCopyHalfScale = mipmap != GX_FALSE;
}

void GXSetDispCopyFrame2Field(u32 mode) {
  g_gxState.dispCopyFrame2Field = mode & 3;
}

void GXSetCopyClamp(GXFBClamp clamp) {
  g_gxState.copyClamp = static_cast<GXFBClamp>(static_cast<u32>(clamp) & 3);
}

u32 GXSetDispCopyYScale(f32 vscale) {
  const u32 iScale = y_scale_to_integer(vscale);
  g_gxState.dispCopyYScale = vscale;
  GX_WRITE_RAS_REG(0x4e000000u | iScale);
  __gx->bpSent = 0;
  return get_num_xfb_lines_internal(static_cast<u16>(g_gxState.dispCopySrc.height), iScale);
}

void GXSetCopyClear(GXColor color, u32 depth) {
  // BP 0x4F: clear color R + A
  u32 reg0 = 0;
  SET_REG_FIELD(0, reg0, 8, 0, color.r);
  SET_REG_FIELD(0, reg0, 8, 8, color.a);
  SET_REG_FIELD(0, reg0, 8, 24, 0x4F);
  GX_WRITE_RAS_REG(reg0);

  // BP 0x50: clear color B + G
  u32 reg1 = 0;
  SET_REG_FIELD(0, reg1, 8, 0, color.b);
  SET_REG_FIELD(0, reg1, 8, 8, color.g);
  SET_REG_FIELD(0, reg1, 8, 24, 0x50);
  GX_WRITE_RAS_REG(reg1);

  // BP 0x51: clear Z (24-bit)
  u32 reg2 = 0;
  SET_REG_FIELD(0, reg2, 24, 0, depth);
  SET_REG_FIELD(0, reg2, 8, 24, 0x51);
  GX_WRITE_RAS_REG(reg2);
  __gx->bpSent = 1;
}

void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {
  g_gxState.copyFilterAa = aa;
  g_gxState.copyFilterVf = vf;
  if (sample_pattern) {
    for (size_t i = 0; i < g_gxState.copyFilterSamplePattern.size(); ++i) {
      g_gxState.copyFilterSamplePattern[i][0] = sample_pattern[i][0];
      g_gxState.copyFilterSamplePattern[i][1] = sample_pattern[i][1];
    }
  }
  if (vfilter) {
    for (size_t i = 0; i < g_gxState.copyFilterVFilter.size(); ++i) {
      g_gxState.copyFilterVFilter[i] = vfilter[i];
    }
  }

  if (!aa) {
    for (auto& sample : g_gxState.copyFilterSamplePattern) {
      sample = {6, 6};
    }
  }
  if (!vf) {
    g_gxState.copyFilterVFilter = {0, 0, 21, 22, 21, 0, 0};
  }

  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x01, g_gxState.copyFilterSamplePattern, 0));
  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x02, g_gxState.copyFilterSamplePattern, 6));
  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x03, g_gxState.copyFilterSamplePattern, 12));
  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x04, g_gxState.copyFilterSamplePattern, 18));
  GX_WRITE_RAS_REG(pack_copy_filter0(g_gxState.copyFilterVFilter));
  GX_WRITE_RAS_REG(pack_copy_filter1(g_gxState.copyFilterVFilter));
  __gx->bpSent = 0;
}

void GXSetDispCopyGamma(GXGamma gamma) {
  g_gxState.dispCopyGamma = static_cast<GXGamma>(static_cast<u32>(gamma) & 3u);
  g_gxState.bpRegCache[0x52] = (g_gxState.bpRegCache[0x52] & ~(3u << 7)) |
                               ((static_cast<u32>(g_gxState.dispCopyGamma) & 3u) << 7);
}

void GXCopyDisp(void* dest, GXBool clear) {
  g_spmBBoxArmed = 0;  // a box cleared but never read this frame stops costing CPU projection
  (void)dest;
  // Finish queued commands before this copy reads live EFB state.
  if (aurora::gx::fifo::get_buffer_size() != 0) {
    aurora::gx::fifo::drain();
  }
  // Round the edges to the nearest render row, as texture copies do. Rounding outward like a scissor
  // pulls the row below the picture into its last line at fractional render scales.
  const auto rect = map_texture_copy_source(g_gxState.dispCopySrc, false).clearRect;
  const auto logicalDstWidth =
      std::max<u32>(g_gxState.dispCopyDstWidth != 0 ? g_gxState.dispCopyDstWidth : static_cast<u32>(g_gxState.dispCopySrc.width), 1);
  const auto logicalDstHeight =
      std::max<u32>(g_gxState.dispCopyDstHeight != 0 ? g_gxState.dispCopyDstHeight : static_cast<u32>(g_gxState.dispCopySrc.height), 1);
  const auto [dstWidth, dstHeight] = scale_copy_dst(logicalDstWidth, logicalDstHeight);

  if (!g_gxState.displayCopyTexture || g_gxState.displayCopyWidth != dstWidth ||
      g_gxState.displayCopyHeight != dstHeight) {
    g_gxState.displayCopyTexture = aurora::gfx::new_render_texture(dstWidth, dstHeight, GX_TF_RGBA8, "Display Copy");
    g_gxState.displayCopyWidth = dstWidth;
    g_gxState.displayCopyHeight = dstHeight;
  }

  const auto clearState = get_copy_clear_state(clear);
  auto copyFilter = combined_copy_filter_coefficients(g_gxState.copyFilterVFilter);
  if (aurora::g_config.disableCopyFilter) {
    copyFilter = {0, copyFilter[0] + copyFilter[1] + copyFilter[2], 0};
  }
  aurora::gfx::resolve_pass(g_gxState.displayCopyTexture, rect, clearState.clearColor, clearState.clearAlpha,
                            clearState.clearDepth, clearState.clearColorValue, aurora::gx::clear_depth_value(),
                            GX_TF_RGBA8, nullptr, false, &copyFilter, false,
                            static_cast<float>(rect.height) / std::max<float>(g_gxState.dispCopySrc.height, 1.0f),
                            (g_gxState.copyClamp & GX_CLAMP_TOP) != 0,
                            (g_gxState.copyClamp & GX_CLAMP_BOTTOM) != 0);
  aurora::gx::set_display_copy_present_source();
}

std::atomic<bool> g_texturePatches{true};
extern "C" void AuroraSetTexturePatches(bool enabled) { g_texturePatches.store(enabled, std::memory_order_relaxed); }

extern "C" int g_spmDrawDump;
void GXCopyTex(void* dest, GXBool clear) {
  g_statCopies.fetch_add(1, std::memory_order_relaxed);
  if (g_spmDrawDump) {  // SPM diagnostic: copies in draw order during the dump frame
    if (aurora::gx::fifo::get_buffer_size() != 0) aurora::gx::fifo::drain();
    std::fprintf(stderr, "[aurora] [2] [aurora::gx::fifo] [spmdraw] COPYTEX src %d,%d %dx%d dst %ux%u clear %d\n",
                 g_gxState.texCopySrc.x, g_gxState.texCopySrc.y, g_gxState.texCopySrc.width, g_gxState.texCopySrc.height,
                 g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight, (int)clear);
  }
  // Texture copies must see all earlier draws and state changes.
  if (aurora::gx::fifo::get_buffer_size() != 0) {
    aurora::gx::fifo::drain();
  }
  auto sourceRect = map_texture_copy_source(g_gxState.texCopySrc, g_gxState.texCopySrcRenderSpace);
  // SPM: with the view widened past 16:9 (Hor+), the game's screen coordinates squeeze toward the centre. A copy
  // of part of the screen (the door-opening effect copies the door) must take the same squeeze to find it;
  // full-width copies stay full-width, since what they're drawn back with covers the whole screen.
  // Brawl: render-to-texture passes (a 512x512 viewport in the corner) were drawn unwidened, so their copies keep
  // their place: only a copy made under a screen-sized viewport takes the squeeze.
  const auto& copyVp = g_gxState.xfViewport;
  const bool screenViewport = copyVp[0] * 2.0f >= 600.0f && -copyVp[1] * 2.0f >= 440.0f;
  if (const float widen = aurora::gx::fifo::g_spmScreenWiden; widen < 1.f && !g_gxState.texCopySrcRenderSpace &&
      screenViewport && g_gxState.texCopySrc.width < static_cast<int32_t>(aurora::gx::logical_fb_size().x)) {
    const float cx = static_cast<float>(aurora::gfx::get_render_target_size().x) * 0.5f;
    auto& sr = sourceRect.sampleRect;
    sr.x() = cx + (sr.x() - cx) * widen;
    sr.z() *= widen;
    auto& cr = sourceRect.clearRect;
    const auto left = static_cast<int32_t>(std::lround(sr.x()));
    cr.width = std::max<int32_t>(static_cast<int32_t>(std::lround(sr.x() + sr.z())) - left, 1);
    cr.x = left;
  }
  const auto rect = sourceRect.clearRect;
  // Keep guest dimensions for cache identity while preserving scaled GPU detail.
  // NSMBW: a destination wider or taller than what the copy writes is a patch into a bigger texture
  // (dNitro's animated tiles copy 32x32 frames into the 1024x1024 tile atlas, the destination size
  // giving the atlas stride). The copy only produces the source's extent, so size the texture to
  // that: a 1024x1024 target per 32x32 patch (2752x1617 at a high render scale) made every such
  // copy allocate tens of megabytes.
  const u32 copiedWidth = static_cast<u32>(std::max(g_gxState.texCopySrc.width, 1)) >> (g_gxState.texCopyHalfScale ? 1 : 0);
  const u32 copiedHeight = static_cast<u32>(std::max(g_gxState.texCopySrc.height, 1)) >> (g_gxState.texCopyHalfScale ? 1 : 0);
  const auto logicalDstWidth = std::max<u32>(std::min<u32>(g_gxState.texCopyDstWidth, std::max<u32>(copiedWidth, 1)), 1);
  const auto logicalDstHeight = std::max<u32>(std::min<u32>(g_gxState.texCopyDstHeight, std::max<u32>(copiedHeight, 1)), 1);
  const auto [scaledDstWidth, scaledDstHeight] = scale_copy_dst(logicalDstWidth, logicalDstHeight);
  const auto texCopyFmt = g_gxState.texCopyFmt;
  const bool sourceHasAlpha = aurora::gx::render_target_has_alpha(g_gxState.pixelFmt);
  const bool forceOpaqueAlpha = !sourceHasAlpha && !aurora::gx::is_depth_format(texCopyFmt);
  const auto resolveFmt = texCopyFmt;

  const aurora::gx::GXState::CopyTextureKey key{
      .dest = dest,
      .width = logicalDstWidth,
      .height = logicalDstHeight,
      .format = texCopyFmt,
  };
  // Keep one live copy per destination and retire stale GPU textures.
  for (auto cacheIt = g_gxState.copyTextureCache.begin(); cacheIt != g_gxState.copyTextureCache.end();) {
    if (cacheIt->first.dest == dest && !(cacheIt->first == key)) {
      g_gxState.copyTextureCache.erase(cacheIt++);
    } else {
      ++cacheIt;
    }
  }
  // NSMBW_LOG_COPIES=<frame>: log every texture copy of that frame and the next (profiling).
  static const long logFrame = [] {
    const char* value = std::getenv("NSMBW_LOG_COPIES");
    return value != nullptr ? std::atol(value) : -1L;
  }();
  const uint64_t createsBefore = g_statCopyCreates.load(std::memory_order_relaxed);
  auto it = g_gxState.copyTextureCache.find(key);
  const bool cacheMiss = it == g_gxState.copyTextureCache.end();
  if (it == g_gxState.copyTextureCache.end()) {
    auto handle = acquire_copy_texture(key, scaledDstWidth, scaledDstHeight, texCopyFmt);
    it = g_gxState.copyTextureCache.emplace(key, aurora::gx::GXState::CopyTextureRef{.handle = handle, .revision = 0}).first;
  }
  auto& handle = it->second;
  const u32 currentFrame = aurora::gfx::current_frame();
  const bool sampledThisFrame = handle.sampledThisFrame && handle.lastSampledFrame == currentFrame;
  const bool scaledSizeChanged = !handle.handle || handle.handle->size.width != scaledDstWidth ||
                                 handle.handle->size.height != scaledDstHeight;
  auto clearState = get_copy_clear_state(clear);
  if (sampledThisFrame || scaledSizeChanged) {
    const u32 revision = handle.revision;
    const u32 lastProducedFrame = handle.lastProducedFrame;
    handle = aurora::gx::GXState::CopyTextureRef{
        .handle = acquire_copy_texture(key, scaledDstWidth, scaledDstHeight, texCopyFmt),
        .revision = revision,
        .lastProducedFrame = lastProducedFrame,
    };
  }
  if (logFrame >= 0 && (currentFrame == static_cast<u32>(logFrame) || currentFrame == static_cast<u32>(logFrame) + 1)) {
    size_t pooled = 0, free = 0;
    for (const auto& entry : g_copyTexturePool) {
      if (entry.key == key) {
        ++pooled;
        free += entry.handle.use_count() == 1;
      }
    }
    std::fprintf(stderr,
                 "[copy] frame %u dest %p %ux%u fmt %d -> %ux%u miss %d sampled %d resized %d created %d pool %zu "
                 "free %zu cache %zu\n",
                 currentFrame, dest, logicalDstWidth, logicalDstHeight, static_cast<int>(texCopyFmt), scaledDstWidth,
                 scaledDstHeight, cacheMiss, sampledThisFrame, scaledSizeChanged,
                 static_cast<int>(g_statCopyCreates.load(std::memory_order_relaxed) - createsBefore), pooled, free,
                 g_gxState.copyTextureCache.size());
  }

  const bool alphaUpdate = g_gxState.alphaUpdate && aurora::gx::render_target_has_alpha(g_gxState.pixelFmt);
  if (alphaUpdate && g_gxState.dstAlpha != UINT32_MAX) {
    if (!clear) {
      // TODO: Confirm how this copy should handle alpha without changing the EFB.
    }
    // Clear alpha with a pipeline that matches the pass sample count.
    aurora::gfx::push_draw_command(aurora::gfx::clear::DrawData{
        .pipeline = aurora::gfx::pipeline_ref(aurora::gfx::clear::PipelineConfig{
            .msaaSamples = aurora::gfx::get_sample_count(),
            .clearColor = false,
            .clearAlpha = true,
            .clearDepth = false,
        }),
        .color = wgpu::Color{0.f, 0.f, 0.f, g_gxState.dstAlpha / 255.f},
    });
  }
  if (aurora::gx::render_target_has_alpha(g_gxState.pixelFmt)) {
    clearState.clearAlpha = clear && alphaUpdate;
  }
  const auto copyFilter = combined_copy_filter_coefficients(g_gxState.copyFilterVFilter);
  // Skip only recurring color copies so one-shot copies are never lost.
  const bool producedConsecutively = handle.revision != 0 && currentFrame - handle.lastProducedFrame <= 1;
  const bool persistentCopy = !aurora::gx::is_depth_format(texCopyFmt) && !producedConsecutively;
  aurora::gfx::resolve_pass(handle.handle, rect, clearState.clearColor, clearState.clearAlpha, clearState.clearDepth,
                            clearState.clearColorValue, aurora::gx::clear_depth_value(), resolveFmt,
                            &sourceRect.sampleRect, g_gxState.texCopyHalfScale, &copyFilter, forceOpaqueAlpha,
                            sourceRect.sampleRect.w() / std::max<float>(g_gxState.texCopySrc.height, 1.0f),
                            (g_gxState.copyClamp & GX_CLAMP_TOP) != 0,
                            (g_gxState.copyClamp & GX_CLAMP_BOTTOM) != 0, persistentCopy);
  ++handle.revision;
  handle.lastProducedFrame = currentFrame;
  handle.width = logicalDstWidth;
  handle.height = logicalDstHeight;
  handle.format = texCopyFmt;
  handle.dataSize = GXGetTexBufferSize(static_cast<u16>(logicalDstWidth), static_cast<u16>(logicalDstHeight), texCopyFmt, GX_FALSE, 0);
  aurora::gx::notify_copy_texture_created();
  g_gxState.copyTextures[dest] = handle;
  // Keep the GPU copy and download it only if guest code reads the destination.
  // A copy smaller than its destination size patches a wider texture (NSMBW's animated tiles: coins,
  // ? blocks, copied into the tile atlas every frame). The patch is applied to the atlas's GPU texture
  // right after this copy resolves. Reading it back into RAM instead (NSMBW_PATCH_READBACK=1) works too
  // but re-decodes the 2 MB atlas every frame: 60 -> 35 FPS on a Snapdragon 6 Gen 1.
  static const bool patchReadback = [] {
    const char* value = std::getenv("NSMBW_PATCH_READBACK");
    return value != nullptr && value[0] == '1';
  }();
  const bool patch = g_gxState.texCopyDstWidth > logicalDstWidth && g_texturePatches.load(std::memory_order_relaxed);
  if (patch && !patchReadback) {
    for (auto& target : aurora::gx::find_patch_targets(dest)) {
      aurora::gfx::add_resolve_patch(handle.handle, std::move(target.texture), target.x, target.y,
                                     logicalDstWidth, logicalDstHeight);
    }
  }
  const u32 patchStride = patch && patchReadback ? g_gxState.texCopyDstWidth : 0;
  aurora::gfx::efb_ram::schedule(dest, logicalDstWidth, logicalDstHeight, texCopyFmt, handle.handle, patchStride);
  // NSMBW: a draw after this copy must not merge into one before it: merged primitives are recorded
  // with the earlier draw and would render before the copy reads the EFB.
  g_gxState.stateDirty = true;
}

void GXClearBoundingBox() {
  g_gxState.boundingBox = {1023, 0, 1023, 0};
  g_spmBBoxArmed = 1;
  GX_WRITE_RAS_REG(0x550003FFu);
  GX_WRITE_RAS_REG(0x560003FFu);
  __gx->bpSent = 0;
}

void GXReadBoundingBox(u16* left, u16* right, u16* top, u16* bottom) {
  g_spmBBoxArmed = 0;
  if (left) {
    *left = g_gxState.boundingBox[0];
  }
  if (right) {
    *right = g_gxState.boundingBox[1];
  }
  if (top) {
    *top = g_gxState.boundingBox[2];
  }
  if (bottom) {
    *bottom = g_gxState.boundingBox[3];
  }
}

u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) {
  return get_num_xfb_lines_internal(efbHeight, y_scale_to_integer(yScale));
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
  CHECK(efbHeight != 0, "GXGetYScaleFactor requires non-zero EFB height");
  CHECK(xfbHeight != 0 && xfbHeight <= 1024, "GXGetYScaleFactor requires 1..1024 XFB lines");

  u32 targetHeight = xfbHeight;
  f32 yScale = static_cast<f32>(targetHeight) / static_cast<f32>(efbHeight);
  u16 realHeight = GXGetNumXfbLines(efbHeight, yScale);

  while (realHeight > xfbHeight && targetHeight > 1) {
    --targetHeight;
    yScale = static_cast<f32>(targetHeight) / static_cast<f32>(efbHeight);
    realHeight = GXGetNumXfbLines(efbHeight, yScale);
  }

  f32 resultScale = yScale;
  while (realHeight < xfbHeight && targetHeight < 1024) {
    resultScale = yScale;
    ++targetHeight;
    yScale = static_cast<f32>(targetHeight) / static_cast<f32>(efbHeight);
    realHeight = GXGetNumXfbLines(efbHeight, yScale);
  }

  return resultScale;
}

}
