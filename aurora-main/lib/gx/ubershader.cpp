#include "ubershader.hpp"

#include "../gfx/common.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "pipeline.hpp"
#include "shader_info.hpp"

#include <dolphin/gx/GXEnum.h>
#include <fmt/format.h>
#ifdef AURORA_WEBGPU_BROWSER
#include <emscripten/em_js.h>
#endif

#include <atomic>
#include <mutex>
#include <string>

namespace aurora::gx {
namespace {
Module Log("aurora::gx::ubershader");

std::atomic<UbershaderMode> g_mode = UbershaderMode::Off;

// Word layout of the packed configuration (pack_ubershader_config); the shader source below is
// generated with the same constants.
constexpr u32 kWordGlobal0 = 0;   // stage count, texgen count, fog, flags, vertex stride
constexpr u32 kWordAlpha = 1;     // alpha compare
constexpr u32 kWordSwap = 2;      // TEV swap tables
constexpr u32 kWordChannels = 3;  // lighting config of COLOR0, COLOR1, ALPHA0, ALPHA1 (a byte each)
constexpr u32 kWordColorSrc = 4;  // vertex color attribute per channel, sampled channels
constexpr u32 kWordGlobal1 = 5;   // NBT index count, indirect stage count
constexpr u32 kWordOffsets = 6;   // uniform member offsets (vec4 units), two per word
constexpr u32 kOffsetCount = 39;
constexpr u32 kWordAttrs = kWordOffsets + (kOffsetCount + 1) / 2; // vertex attributes, GX_VA order
constexpr u32 kAttrCount = GX_VA_TEX7 + 1;
constexpr u32 kWordTcgs = kWordAttrs + kAttrCount; // texgens
constexpr u32 kWordInd = kWordTcgs + MaxTexCoord;  // indirect stages
constexpr u32 kWordStages = kWordInd + MaxIndStages;
constexpr u32 kWordsPerStage = 4;
static_assert(kWordStages + kWordsPerStage * MaxTevStages <= MaxUberConfigWords);

// Uniform member offsets, in build_uniform's order.
enum UniformOffset : u32 {
  kOffPostex = 0,
  kOffNrm = 1,
  kOffTevReg = 2,        // 4: TEVPREV, TEVREG0-2
  kOffLights = 6,
  kOffLightState = 7,
  kOffChannelColor = 8,  // 8: channel * 4 + {amb, mat, alpha amb, alpha mat}
  kOffKColor = 16,       // 4
  kOffPostMtx = 20,
  kOffFog = 21,
  kOffIndMtx = 22,
  kOffTexCoordScale = 23, // 8
  kOffTexSize = 31,       // 8
};
static_assert(kOffTexSize + MaxTextures == kOffsetCount);
constexpr u32 kAbsent = 0xFFFF;

// The uniform header (vtx_start .. array_start) is 80 bytes and the projection 64: five and four vec4.
constexpr u32 kHeaderVec4 = 5;
constexpr u32 kProjectionVec4 = 4;

u32 bits(u32 value, u32 offset) { return value << offset; }

bool color_arg_supported(GXTevColorArg arg) { return arg <= GX_CC_ZERO; }

int ind_scale_shift(GXIndTexScale scale) {
  switch (scale) {
  case GX_ITS_1:
    return 0;
  case GX_ITS_2:
    return 1;
  case GX_ITS_4:
    return 2;
  case GX_ITS_8:
    return 3;
  case GX_ITS_16:
    return 4;
  case GX_ITS_32:
    return 5;
  case GX_ITS_64:
    return 6;
  case GX_ITS_128:
    return 7;
  case GX_ITS_256:
    return 8;
  default:
    return -1;
  }
}

// GXIndTexWrap -> 0 off, 1..5 a (256 >> (code - 1)) << 7 mask, 6 zero; -1 when unknown.
int wrap_code(GXIndTexWrap wrap) {
  const int mask = tev_indirect_wrap_mask(wrap);
  if (mask == -1) return 0;
  if (mask == 0) return 6;
  for (int code = 1; code <= 5; ++code) {
    if (mask == ((256 >> (code - 1)) << 7) - 1) return code;
  }
  return -1;
}

// GX fog type -> 0 none, 1 linear, 2 exp, 3 exp2, 4 reverse exp, 5 reverse exp2 (shader.cpp's switch).
int fog_function(u8 fogType) {
  switch (fogType) {
  case GX_FOG_NONE:
    return 0;
  case 1:
  case 3:
  case GX_FOG_PERSP_LIN:
  case 9:
  case 11:
  case GX_FOG_ORTHO_LIN:
    return 1;
  case GX_FOG_PERSP_EXP:
  case GX_FOG_ORTHO_EXP:
    return 2;
  case GX_FOG_PERSP_EXP2:
  case GX_FOG_ORTHO_EXP2:
    return 3;
  case GX_FOG_PERSP_REVEXP:
  case GX_FOG_ORTHO_REVEXP:
    return 4;
  case GX_FOG_PERSP_REVEXP2:
  case GX_FOG_ORTHO_REVEXP2:
    return 5;
  default:
    return -1;
  }
}
} // namespace

#ifdef AURORA_WEBGPU_BROWSER
// The embedding page picks the mode with Module.mkwUbershader ('hybrid' or 'all'; WiiCompiled:
// ?uber, ?uber=all) and asks for deferred compiles with Module.mkwDeferPipelines (WebKit).
EM_JS(int, aurora_web_ubershader_requested, (), {
  const mode = Module['mkwUbershader'];
  return mode === 'all' ? 2 : mode ? 1 : 0;
});
EM_JS(int, aurora_web_defer_pipelines_requested, (), { return Module['mkwDeferPipelines'] ? 1 : 0; });
#endif

UbershaderMode ubershader_mode() noexcept {
#ifdef AURORA_WEBGPU_BROWSER
  static const int requested = aurora_web_ubershader_requested();
  if (requested != 0) return requested == 2 ? UbershaderMode::All : UbershaderMode::Hybrid;
#endif
  return g_mode.load(std::memory_order_relaxed);
}
void set_ubershader_mode(UbershaderMode mode) noexcept { g_mode.store(mode, std::memory_order_relaxed); }

bool defer_specialised_pipelines() noexcept {
#ifdef AURORA_WEBGPU_BROWSER
  static const bool requested = aurora_web_defer_pipelines_requested() != 0;
  return requested && ubershader_mode() == UbershaderMode::Hybrid;
#else
  return false;
#endif
}

ShaderConfig ubershader_marker_config() noexcept {
  ShaderConfig marker{};
  // No real configuration has zero TEV stages together with every z-texture bit set.
  marker.tevStageCount = 0;
  marker.zTexture = 0xFFFFFFFFu;
  return marker;
}

bool is_ubershader_marker(const ShaderConfig& config) noexcept {
  return config.tevStageCount == 0 && config.zTexture == 0xFFFFFFFFu;
}

UberConfig pack_ubershader_config(const ShaderConfig& config, const ShaderInfo& info) noexcept {
  // Coverage: what the specialised shaders handle that this one leaves to them.
  if (config.lineMode != 0 || tev_z_texture_enabled(config) || config.tevStageCount == 0 ||
      config.tevStageCount > MaxTevStages || config.numTexGens > MaxTexCoord || config.numIndStages > MaxIndStages) {
    return {};
  }
  const int fogFn = fog_function(config.fogType);
  if (fogFn < 0) return {};
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const bool regular = [](GXTevOp op) { return op == GX_TEV_ADD || op == GX_TEV_SUB; }(stage.colorOp.op) &&
                         [](GXTevOp op) { return op == GX_TEV_ADD || op == GX_TEV_SUB; }(stage.alphaOp.op);
    if (!regular || !color_arg_supported(stage.colorPass.a) || !color_arg_supported(stage.colorPass.b) ||
        !color_arg_supported(stage.colorPass.c) || !color_arg_supported(stage.colorPass.d) ||
        stage.alphaPass.a > GX_CA_ZERO || stage.alphaPass.b > GX_CA_ZERO || stage.alphaPass.c > GX_CA_ZERO ||
        stage.alphaPass.d > GX_CA_ZERO || stage.colorOp.outReg > GX_TEVREG2 || stage.alphaOp.outReg > GX_TEVREG2 ||
        stage.colorOp.bias > GX_TB_SUBHALF || stage.alphaOp.bias > GX_TB_SUBHALF ||
        stage.colorOp.scale > GX_CS_DIVIDE_2 || stage.alphaOp.scale > GX_CS_DIVIDE_2 || stage.kcSel > 0x1F ||
        stage.kaSel > 0x1F) {
      return {};
    }
  }
  for (u32 attr = 0; attr < kAttrCount; ++attr) {
    const auto& mapping = config.attrs[attr];
    if (mapping.attrType == GX_NONE) continue;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      if (mapping.compType > GX_RGBA8) return {};
    } else if (attr >= GX_VA_POS && mapping.compType > GX_F32) {
      return {};
    }
  }

  UberConfig out;
  auto& w = out.words;

  // Uniform member offsets: build_uniform's layout for this ShaderInfo.
  std::array<u32, kOffsetCount> offs;
  offs.fill(kAbsent);
  u32 next = kHeaderVec4 + kProjectionVec4;
  const auto& layout = info.matrixLayout;
  offs[kOffPostex] = next;
  next += 3 * layout.postexCount;
  offs[kOffNrm] = next;
  next += 3 * layout.nrmCount;
  const auto loadsTevRegs = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  for (u32 reg = 0; reg < MaxTevRegs; ++reg) {
    if (loadsTevRegs.test(reg)) offs[kOffTevReg + reg] = next++;
  }
  if (info.lightingEnabled) {
    static_assert(sizeof(Light) == 5 * 16);
    offs[kOffLights] = next;
    next += 5 * GX::MaxLights;
    offs[kOffLightState] = next++;
  }
  for (u32 channel = 0; channel < info.sampledColorChannels.size(); ++channel) {
    if (!info.sampledColorChannels.test(channel)) continue;
    const auto& cc = config.colorChannels[channel];
    const auto& cca = config.colorChannels[channel + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) offs[kOffChannelColor + channel * 4 + 0] = next++;
    if (cc.matSrc == GX_SRC_REG) offs[kOffChannelColor + channel * 4 + 1] = next++;
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) offs[kOffChannelColor + channel * 4 + 2] = next++;
    if (cca.matSrc == GX_SRC_REG) offs[kOffChannelColor + channel * 4 + 3] = next++;
  }
  for (u32 k = 0; k < MaxKColors; ++k) {
    if (info.sampledKColors.test(k)) offs[kOffKColor + k] = next++;
  }
  if (info.usesPTTexMtx.any()) {
    offs[kOffPostMtx] = next;
    next += 3 * MaxPTTexMtx;
  }
  if (info.usesFog) {
    static_assert(sizeof(Fog) == 6 * 16);
    offs[kOffFog] = next;
    next += 6;
  }
  if (info.usedIndTexMtxs.any()) {
    offs[kOffIndMtx] = next;
    next += MaxIndTexMtxs * 2;
  }
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    if (info.sampledTexCoords.test(i)) offs[kOffTexCoordScale + i] = next++;
  }
  for (u32 i = 0; i < MaxTextures; ++i) {
    if (info.sampledTextures.test(i)) offs[kOffTexSize + i] = next++;
  }
  if (next * 16 > info.uniformSize) {
    Log.error("uniform layout walk ({} bytes) does not match build_shader_info ({} bytes)", next * 16,
              info.uniformSize);
    return {};
  }
  for (u32 i = 0; i < kOffsetCount; ++i) {
    w[kWordOffsets + i / 2] |= offs[i] << ((i & 1) * 16);
  }

  const bool fixedTexState = shader_uses_fixed_texcoord_state(config);
  const u32 alphaMode = config.alphaCompare ? 1 : 0;
  w[kWordGlobal0] = bits(config.tevStageCount, 0) | bits(config.numTexGens, 5) | bits(fogFn, 9) |
                    bits((config.fogType & 0x8u) != 0, 12) | bits(config.fogRangeAdjust, 13) |
                    bits(config.dualTexEnabled, 14) | bits(fixedTexState, 15) | bits(config.numTexGens != 0, 16) |
                    bits(alphaMode, 17) | bits(config.vtxStride, 24);
  w[kWordAlpha] = bits(config.alphaCompare.comp0 & 7u, 0) | bits(config.alphaCompare.ref0 & 0xFFu, 3) |
                  bits(config.alphaCompare.op & 3u, 11) | bits(config.alphaCompare.comp1 & 7u, 13) |
                  bits(config.alphaCompare.ref1 & 0xFFu, 16);
  for (u32 t = 0; t < MaxTevSwap; ++t) {
    const auto& swap = config.tevSwapTable[t];
    w[kWordSwap] |= bits((swap.red & 3u) | ((swap.green & 3u) << 2) | ((swap.blue & 3u) << 4) |
                             ((swap.alpha & 3u) << 6),
                         t * 8);
  }
  for (u32 c = 0; c < MaxColorChannels; ++c) {
    const auto& cc = config.colorChannels[c];
    const u32 diff = cc.diffFn == GX_DF_SIGN ? 1 : cc.diffFn == GX_DF_CLAMP ? 2 : 0;
    const u32 attn = cc.attnFn == GX_AF_SPOT ? 1 : cc.attnFn == GX_AF_SPEC ? 2 : 0;
    const u32 byte = bits(cc.lightingEnabled, 0) | bits(cc.matSrc == GX_SRC_VTX, 1) | bits(cc.ambSrc == GX_SRC_VTX, 2) |
                     bits(diff, 3) | bits(attn, 5);
    w[kWordChannels] |= byte << (c * 8);
  }
  for (u32 c = 0; c < 2; ++c) {
    const int attr = shader_vertex_color_attr(config, c);
    const u32 code = attr == GX_VA_CLR0 ? 1 : attr == GX_VA_CLR1 ? 2 : 0;
    w[kWordColorSrc] |= bits(code, c * 2);
  }
  w[kWordColorSrc] |= bits(static_cast<u32>(info.sampledColorChannels.to_ulong()) & 3u, 4);
  w[kWordGlobal1] = bits(config.attrs[GX_VA_NRM].nrmIndexCount & 3u, 0) | bits(config.numIndStages, 2);

  for (u32 attr = 0; attr < kAttrCount; ++attr) {
    const auto& m = config.attrs[attr];
    if (m.attrType == GX_NONE) continue;
    w[kWordAttrs + attr] = bits(m.attrType & 3u, 0) | bits(m.cnt & 0xFu, 2) | bits(m.compType & 0xFu, 6) |
                           bits(m.frac & 0x1Fu, 10) | bits(m.le, 15) | bits(m.offset, 16) | bits(m.stride, 24);
  }

  for (u32 i = 0; i < config.numTexGens; ++i) {
    if (!info.sampledTexCoords.test(i)) continue;
    const auto& tcg = config.tcgs[i];
    u32 type;
    if (tcg.type == GX_TG_MTX3x4) type = 0;
    else if (tcg.type == GX_TG_MTX2x4) type = 1;
    else if (tcg.type == GX_TG_SRTG) type = 2;
    else return {};
    u32 src;
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) src = 4 + (tcg.src - GX_TG_TEX0);
    else if (tcg.src == GX_MAX_TEXGENSRC) src = 4 + i;
    else if (tcg.src == GX_TG_POS) src = 0;
    else if (tcg.src == GX_TG_NRM) src = 1;
    else if (tcg.src == GX_TG_BINRM) src = 2;
    else if (tcg.src == GX_TG_TANGENT) src = 3;
    else if (tcg.src == GX_TG_COLOR0) src = 12;
    else if (tcg.src == GX_TG_COLOR1) src = 13;
    else return {};
    u32 mtxMode = 0;
    u32 mtxIndex = 0;
    if (type != 2) {
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        mtxMode = 2;
      } else if (tcg.mtx != GX_IDENTITY) {
        const u32 slot = static_cast<u32>(tcg.mtx) / 3;
        const u32 index = slot < MaxPostexMtx ? layout.postexRemap[slot] : slot;
        if (index == UniformMatrixLayout::kAbsent || index >= 32) return {};
        mtxMode = 1;
        mtxIndex = index;
      }
    }
    const bool postIdentity = tcg.postMtx == GX_PTIDENTITY;
    const u32 postIndex = postIdentity ? 0 : (tcg.postMtx - GX_PTTEXMTX0) / 3;
    if (config.dualTexEnabled && !postIdentity && postIndex >= MaxPTTexMtx) return {};
    w[kWordTcgs + i] = bits(type, 0) | bits(src, 2) | bits(mtxMode, 7) | bits(mtxIndex, 9) | bits(postIdentity, 14) |
                       bits(postIndex & 0x1Fu, 15) | bits(tcg.normalize, 20) | bits(tcg.inputFormAB11, 21) |
                       bits(1u, 22);
  }

  for (u32 i = 0; i < MaxIndStages; ++i) {
    if (!info.usedIndStages.test(i)) continue;
    const auto& ind = config.indStages[i];
    const int texCoord = tev_effective_texcoord(config, ind.texCoordId);
    const int scaleS = ind_scale_shift(ind.scaleS);
    const int scaleT = ind_scale_shift(ind.scaleT);
    if (ind.texMapId >= MaxTextures || scaleS < 0 || scaleT < 0) return {};
    w[kWordInd + i] = bits(1u, 0) | bits(texCoord >= 0, 1) | bits(texCoord >= 0 ? texCoord : 0, 2) |
                      bits(ind.texMapId, 5) | bits(scaleS, 8) | bits(scaleT, 12);
  }

  for (u32 s = 0; s < config.tevStageCount; ++s) {
    const auto& stage = config.tevStages[s];
    const auto dep = tev_stage_texture_dependency(config, s);
    const bool willSample = tev_texture_sample_enabled(dep, dep.combinerUsesTexture);
    const bool hasBaseCoord = dep.texCoordId >= 0;

    u32 rasKind = 0;
    bool bumpN = false;
    u32 bumpSel = 0;
    u32 bumpMask = 0;
    if (stage.channelId == GX_ALPHA_BUMP || stage.channelId == GX_ALPHA_BUMPN) {
      rasKind = 3;
      bumpN = stage.channelId == GX_ALPHA_BUMPN;
      if (stage.indTexStage < config.numIndStages && stage.indTexAlphaSel != GX_ITBA_OFF) {
        bumpSel = stage.indTexAlphaSel == GX_ITBA_S ? 1 : stage.indTexAlphaSel == GX_ITBA_T ? 2 : 3;
        bumpMask = stage.indTexFormat == GX_ITF_5 ? 1 : stage.indTexFormat == GX_ITF_4 ? 2 : 0;
      }
    } else if (stage.channelId != GX_COLOR_ZERO && stage.channelId != GX_COLOR_NULL) {
      size_t channel = 0;
      switch (stage.channelId) {
      case GX_COLOR0:
      case GX_ALPHA0:
      case GX_COLOR0A0:
        channel = 0;
        break;
      case GX_COLOR1:
      case GX_ALPHA1:
      case GX_COLOR1A1:
        channel = 1;
        break;
      default:
        return {};
      }
      rasKind = 1 + channel;
    }

    const bool hasIndirectStage = stage.indTexStage < config.numIndStages;
    const bool indActive = stage.indTexMtxId != GX_ITM_OFF && hasIndirectStage;
    u32 fmtShift = 0;
    switch (stage.indTexFormat) {
    case GX_ITF_8:
      break;
    case GX_ITF_5:
      fmtShift = 3;
      break;
    case GX_ITF_4:
      fmtShift = 4;
      break;
    case GX_ITF_3:
      fmtShift = 5;
      break;
    default:
      if (indActive) return {};
    }
    const u32 biasSel = stage.indTexBiasSel;
    const bool biasS = biasSel == GX_ITB_S || biasSel == GX_ITB_ST || biasSel == GX_ITB_SU || biasSel == GX_ITB_STU;
    const bool biasT = biasSel == GX_ITB_T || biasSel == GX_ITB_ST || biasSel == GX_ITB_TU || biasSel == GX_ITB_STU;
    const bool biasU = biasSel == GX_ITB_U || biasSel == GX_ITB_SU || biasSel == GX_ITB_TU || biasSel == GX_ITB_STU;
    u32 mtxKind = 0;
    u32 mtxIdx = 0;
    if (indActive) {
      if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
        mtxKind = 1;
        mtxIdx = stage.indTexMtxId - GX_ITM_0;
      } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
        mtxKind = 2;
        mtxIdx = stage.indTexMtxId - GX_ITM_S0;
      } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
        mtxKind = 3;
        mtxIdx = stage.indTexMtxId - GX_ITM_T0;
      }
    }
    const int wrapS = wrap_code(stage.indTexWrapS);
    const int wrapT = wrap_code(stage.indTexWrapT);
    if (dep.needsFixedTexcoordState && (wrapS < 0 || wrapT < 0)) return {};

    const u32 base = kWordStages + s * kWordsPerStage;
    w[base + 0] = bits(stage.colorPass.a, 0) | bits(stage.colorPass.b, 4) | bits(stage.colorPass.c, 8) |
                  bits(stage.colorPass.d, 12) | bits(stage.alphaPass.a, 16) | bits(stage.alphaPass.b, 19) |
                  bits(stage.alphaPass.c, 22) | bits(stage.alphaPass.d, 25);
    const auto op = [](const TevOp& o) {
      return bits(o.op == GX_TEV_SUB, 0) | bits(o.bias, 1) | bits(o.scale, 3) | bits(o.clamp, 5) | bits(o.outReg, 6);
    };
    w[base + 1] = op(stage.colorOp) | (op(stage.alphaOp) << 8) | bits(stage.kcSel, 16) | bits(stage.kaSel, 21);
    w[base + 2] = bits(willSample, 0) | bits(dep.canSampleTexture, 1) | bits(dep.texMapId >= 0 ? dep.texMapId : 0, 2) |
                  bits(dep.texCoordId >= 0, 5) | bits(dep.texCoordId >= 0 ? dep.texCoordId : 0, 6) |
                  bits(dep.needsFixedTexcoordState, 9) | bits(rasKind, 10) | bits(bumpN, 12) |
                  bits(stage.tevSwapRas & 3u, 13) | bits(stage.tevSwapTex & 3u, 15) | bits(hasBaseCoord, 17) |
                  bits(bumpSel, 18) | bits(bumpMask, 20);
    w[base + 3] = bits(indActive, 0) | bits(stage.indTexStage & 3u, 1) | bits(fmtShift, 3) | bits(biasS, 6) |
                  bits(biasT, 7) | bits(biasU, 8) | bits(stage.indTexFormat == GX_ITF_8, 9) | bits(mtxKind, 10) |
                  bits(mtxIdx, 12) | bits(wrapS < 0 ? 0 : wrapS, 14) | bits(wrapT < 0 ? 0 : wrapT, 17) |
                  bits(stage.indTexAddPrev, 20);
  }

  out.wordCount = AURORA_ALIGN(kWordStages + kWordsPerStage * config.tevStageCount, 4u);
  if (info.uniformSize + out.wordCount * 4 > MaxUniformSize) return {};
  return out;
}

namespace {
// The WGSL body; ubershader_source() prepends the word layout as constants.
constexpr std::string_view kUberShaderBody = R"WGSL(
@group(0) @binding(0) var<storage, read> vbuf: array<u32>;
@group(0) @binding(1) var<storage, read> abuf: array<u32>;
@group(1) @binding(0) var<uniform> ubuf: array<vec4u, UNIFORM_VEC4S>;
@group(2) @binding(0) var tex0: texture_2d<f32>;
@group(2) @binding(1) var tex0_samp: sampler;
@group(2) @binding(2) var tex1: texture_2d<f32>;
@group(2) @binding(3) var tex1_samp: sampler;
@group(2) @binding(4) var tex2: texture_2d<f32>;
@group(2) @binding(5) var tex2_samp: sampler;
@group(2) @binding(6) var tex3: texture_2d<f32>;
@group(2) @binding(7) var tex3_samp: sampler;
@group(2) @binding(8) var tex4: texture_2d<f32>;
@group(2) @binding(9) var tex4_samp: sampler;
@group(2) @binding(10) var tex5: texture_2d<f32>;
@group(2) @binding(11) var tex5_samp: sampler;
@group(2) @binding(12) var tex6: texture_2d<f32>;
@group(2) @binding(13) var tex6_samp: sampler;
@group(2) @binding(14) var tex7: texture_2d<f32>;
@group(2) @binding(15) var tex7_samp: sampler;

// --- The draw's uniform data, read as vec4u -------------------------------------------------
fn uw(i: u32) -> u32 { return ubuf[i >> 2u][i & 3u]; }
fn uf4(v: u32) -> vec4f { return bitcast<vec4f>(ubuf[v]); }
// The packed configuration's position (vec4 units) is in the header's pad.x.
fn cfgw(i: u32) -> u32 { return uw(ubuf[1u].z * 4u + i); }
fn uoff(k: u32) -> u32 { return extractBits(cfgw(W_OFFSETS + (k >> 1u)), (k & 1u) * 16u, 16u); }
fn uf4_opt(k: u32) -> vec4f {
  let o = uoff(k);
  if (o == 0xFFFFu) { return vec4f(0.0); }
  return uf4(o);
}
fn mtx34(o: u32) -> mat3x4f { return mat3x4f(uf4(o), uf4(o + 1u), uf4(o + 2u)); }
fn postex(i: u32) -> mat3x4f { return mtx34(uoff(OFF_POSTEX) + 3u * i); }
fn nrm_mtx(i: u32) -> mat3x4f { return mtx34(uoff(OFF_NRM) + 3u * i); }
fn proj_mtx() -> mat4x4f { return mat4x4f(uf4(5u), uf4(6u), uf4(7u), uf4(8u)); }

// --- Vertex fetch: the vertex buffer is big endian, arrays use their own byte order ----------
fn rd(arr: bool, word: u32) -> u32 {
  if (arr) { return abuf[word]; }
  return vbuf[word];
}
fn ld8(arr: bool, off: u32) -> u32 { return (rd(arr, off >> 2u) >> ((off & 3u) * 8u)) & 0xFFu; }
fn ld16(arr: bool, off: u32, le: bool) -> u32 {
  let b0 = ld8(arr, off);
  let b1 = ld8(arr, off + 1u);
  return select((b0 << 8u) | b1, b0 | (b1 << 8u), le);
}
fn ld24(arr: bool, off: u32, le: bool) -> u32 {
  let b0 = ld8(arr, off);
  let b1 = ld8(arr, off + 1u);
  let b2 = ld8(arr, off + 2u);
  return select((b0 << 16u) | (b1 << 8u) | b2, b0 | (b1 << 8u) | (b2 << 16u), le);
}
fn ld32(arr: bool, off: u32, le: bool) -> u32 {
  let b0 = ld8(arr, off);
  let b1 = ld8(arr, off + 1u);
  let b2 = ld8(arr, off + 2u);
  let b3 = ld8(arr, off + 3u);
  return select((b0 << 24u) | (b1 << 16u) | (b2 << 8u) | b3, b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u), le);
}

struct AttrLoc {
  arr: bool,
  off: u32,
  le: bool,
}

fn attr_word(a: u32) -> u32 { return cfgw(W_ATTRS + a); }
fn attr_present(a: u32) -> bool { return (attr_word(a) & 3u) != 0u; }
fn vtx_stride() -> u32 { return extractBits(cfgw(W_GLOBAL0), 24u, 8u); }

// `index_bytes`: extra offset of the index itself (NBT normals with three indices).
fn attr_loc(a: u32, vidx: u32, index_bytes: u32) -> AttrLoc {
  let w = attr_word(a);
  let kind = w & 3u;
  let off = ubuf[0u].x + vidx * vtx_stride() + extractBits(w, 16u, 8u) + index_bytes;
  let stride = extractBits(w, 24u, 8u);
  let le = (w & 0x8000u) != 0u;
  if (kind == 2u) {
    return AttrLoc(true, uw(8u + a - A_POS) + ld8(false, off) * stride, le);
  }
  if (kind == 3u) {
    return AttrLoc(true, uw(8u + a - A_POS) + ld16(false, off, false) * stride, le);
  }
  return AttrLoc(false, off, false);
}

fn comp_size(ctype: u32) -> u32 {
  if (ctype <= 1u) { return 1u; }
  if (ctype <= 3u) { return 2u; }
  return 4u;
}

fn comp(loc: AttrLoc, i: u32, ctype: u32, frac: u32) -> f32 {
  let scale = f32(1u << frac);
  switch ctype {
    case 0u: { return f32(ld8(loc.arr, loc.off + i)) / scale; }
    case 1u: { return f32((bitcast<i32>(ld8(loc.arr, loc.off + i)) << 24u) >> 24u) / scale; }
    case 2u: { return f32(ld16(loc.arr, loc.off + i * 2u, loc.le)) / scale; }
    case 3u: { return f32(bitcast<i32>(ld16(loc.arr, loc.off + i * 2u, loc.le) << 16u) >> 16u) / scale; }
    default: { return bitcast<f32>(ld32(loc.arr, loc.off + i * 4u, loc.le)); }
  }
}

fn load_pos(vidx: u32) -> vec3f {
  let w = attr_word(A_POS);
  let loc = attr_loc(A_POS, vidx, 0u);
  let ct = extractBits(w, 6u, 4u);
  let frac = extractBits(w, 10u, 5u);
  let x = comp(loc, 0u, ct, frac);
  let y = comp(loc, 1u, ct, frac);
  if (extractBits(w, 2u, 4u) == 2u) { return vec3f(x, y, 0.0); }
  return vec3f(x, y, comp(loc, 2u, ct, frac));
}

// Normal, or one of the three NBT groups (shader.cpp normal_group_load).
fn load_nrm_group(vidx: u32, group: u32) -> vec3f {
  let w = attr_word(A_NRM);
  let kind = w & 3u;
  if (kind == 0u) { return vec3f(1.0, 0.0, 0.0); }
  let ct = extractBits(w, 6u, 4u);
  let frac = extractBits(w, 10u, 5u);
  var g = group;
  if (extractBits(w, 2u, 4u) != 9u) { g = 0u; }
  let nrm_indices = extractBits(cfgw(W_GLOBAL1), 0u, 2u);
  let indexed3 = nrm_indices == 3u && (kind == 2u || kind == 3u);
  var index_bytes = 0u;
  if (nrm_indices == 3u && kind == 2u) { index_bytes = g; }
  if (nrm_indices == 3u && kind == 3u) { index_bytes = g * 2u; }
  var loc = attr_loc(A_NRM, vidx, index_bytes);
  if (!indexed3) { loc.off = loc.off + g * 3u * comp_size(ct); }
  return vec3f(comp(loc, 0u, ct, frac), comp(loc, 1u, ct, frac), comp(loc, 2u, ct, frac));
}

fn load_clr(c: u32, vidx: u32) -> vec4f {
  let a = A_CLR0 + c;
  if (!attr_present(a)) { return vec4f(1.0); }
  let loc = attr_loc(a, vidx, 0u);
  switch extractBits(attr_word(a), 6u, 4u) {
    case 0u: {
      let v = ld16(loc.arr, loc.off, loc.le);
      return vec4f(f32((v >> 11u) & 0x1Fu) / 31.0, f32((v >> 5u) & 0x3Fu) / 63.0, f32(v & 0x1Fu) / 31.0, 1.0);
    }
    case 1u: {
      return vec4f(f32(ld8(loc.arr, loc.off)), f32(ld8(loc.arr, loc.off + 1u)), f32(ld8(loc.arr, loc.off + 2u)), 255.0) / 255.0;
    }
    case 2u: {
      return vec4f(f32(ld8(loc.arr, loc.off)), f32(ld8(loc.arr, loc.off + 1u)), f32(ld8(loc.arr, loc.off + 2u)), 255.0) / 255.0;
    }
    case 3u: {
      let v = ld16(loc.arr, loc.off, loc.le);
      return vec4f(f32((v >> 12u) & 0xFu), f32((v >> 8u) & 0xFu), f32((v >> 4u) & 0xFu), f32(v & 0xFu)) / 15.0;
    }
    case 4u: {
      let v = ld24(loc.arr, loc.off, loc.le);
      return vec4f(f32((v >> 18u) & 0x3Fu), f32((v >> 12u) & 0x3Fu), f32((v >> 6u) & 0x3Fu), f32(v & 0x3Fu)) / 63.0;
    }
    default: {
      return vec4f(f32(ld8(loc.arr, loc.off)), f32(ld8(loc.arr, loc.off + 1u)), f32(ld8(loc.arr, loc.off + 2u)),
                   f32(ld8(loc.arr, loc.off + 3u))) / 255.0;
    }
  }
}

fn load_tex(k: u32, vidx: u32) -> vec2f {
  let a = A_TEX0 + k;
  let w = attr_word(a);
  if ((w & 3u) == 0u) { return vec2f(0.0); }
  let loc = attr_loc(a, vidx, 0u);
  let ct = extractBits(w, 6u, 4u);
  let frac = extractBits(w, 10u, 5u);
  let s = comp(loc, 0u, ct, frac);
  if (extractBits(w, 2u, 4u) == 1u) { return vec2f(s, 0.0); }
  return vec2f(s, comp(loc, 1u, ct, frac));
}

// --- Lighting (per vertex: UsePerPixelLighting is off) ------------------------------------
fn chan(c: u32) -> u32 { return extractBits(cfgw(W_CHANNELS), c * 8u, 8u); }

fn vertex_color(ch: u32, clr0: vec4f, clr1: vec4f) -> vec4f {
  let code = extractBits(cfgw(W_COLOR_SRC), ch * 2u, 2u);
  if (code == 1u) { return clr0; }
  if (code == 2u) { return clr1; }
  return vec4f(1.0);
}

fn light_term(i: u32, diff_fn: u32, attn_fn: u32, mv_pos: vec3f, mv_nrm: vec3f) -> f32 {
  let base = uoff(OFF_LIGHTS) + i * 5u;
  let light_pos = uf4(base).xyz;
  let light_dir = uf4(base + 1u).xyz;
  let cos_att = uf4(base + 3u).xyz;
  let dist_att = uf4(base + 4u).xyz;
  var ldir = light_pos - mv_pos;
  let dist2 = dot(ldir, ldir);
  let dist = sqrt(dist2);
  var attn: f32;
  if (attn_fn == 0u) {
    ldir = select(mv_nrm, ldir / max(dist, 1e-20), dist > 0.0);
    attn = 1.0;
  } else {
    ldir = ldir / dist;
    if (attn_fn == 1u) {
      let cosine = max(0.0, dot(ldir, light_dir));
      let cos_attn = dot(cos_att, vec3f(1.0, cosine, cosine * cosine));
      let dist_attn = dot(dist_att, vec3f(1.0, dist, dist2));
      attn = max(0.0, cos_attn / dist_attn);
    } else {
      attn = select(0.0, max(0.0, dot(mv_nrm, light_dir)), dot(mv_nrm, ldir) >= 0.0);
      let cos_attn = dot(cos_att, vec3f(1.0, attn, attn * attn));
      var dist_attn: f32;
      if (diff_fn != 0u) {
        dist_attn = dot(normalize(dist_att), vec3f(1.0, attn, attn * attn));
      } else {
        dist_attn = dot(dist_att, vec3f(1.0, attn, attn * attn));
      }
      attn = select(select(0.0, 1.0, cos_attn > 0.0), max(0.0, cos_attn / dist_attn), dist_attn != 0.0);
    }
  }
  var diff = 1.0;
  if (diff_fn == 1u) {
    diff = dot(ldir, mv_nrm);
  } else if (diff_fn == 2u) {
    diff = max(0.0, dot(ldir, mv_nrm));
  }
  return attn * diff;
}

fn lit_rgb(ch: u32, mv_pos: vec3f, mv_nrm: vec3f, vcol: vec4f) -> vec3f {
  let c = chan(ch);
  var mat = uf4_opt(OFF_CHANNEL_COLOR + ch * 4u + 1u);
  if ((c & 2u) != 0u) { mat = vcol; }
  if ((c & 1u) == 0u) { return vec3f(vec3i(round(mat.rgb * 255.0))) / 255.0; }
  var amb = uf4_opt(OFF_CHANNEL_COLOR + ch * 4u);
  if ((c & 4u) != 0u) { amb = vcol; }
  let mask = uw(uoff(OFF_LIGHT_STATE) * 4u + ch);
  var lighting = vec3i(round(amb.rgb * 255.0));
  for (var i = 0u; i < 8u; i++) {
    if ((mask & (1u << i)) == 0u) { continue; }
    let term = light_term(i, extractBits(c, 3u, 2u), extractBits(c, 5u, 2u), mv_pos, mv_nrm);
    lighting = lighting + vec3i(round(term * uf4(uoff(OFF_LIGHTS) + i * 5u + 2u).rgb * 255.0));
  }
  let lacc = clamp(lighting, vec3i(0), vec3i(255));
  let material = vec3i(round(mat.rgb * 255.0));
  return vec3f((material * (lacc + (lacc >> vec3u(7u)))) >> vec3u(8u)) / 255.0;
}

fn lit_alpha(ch: u32, mv_pos: vec3f, mv_nrm: vec3f, vcol: vec4f) -> f32 {
  let c = chan(ch + 2u);
  var mat = uf4_opt(OFF_CHANNEL_COLOR + ch * 4u + 3u);
  if ((c & 2u) != 0u) { mat = vcol; }
  if ((c & 1u) == 0u) { return f32(i32(round(mat.a * 255.0))) / 255.0; }
  var amb = uf4_opt(OFF_CHANNEL_COLOR + ch * 4u + 2u);
  if ((c & 4u) != 0u) { amb = vcol; }
  let mask = uw(uoff(OFF_LIGHT_STATE) * 4u + 2u + ch);
  var lighting = i32(round(amb.a * 255.0));
  for (var i = 0u; i < 8u; i++) {
    if ((mask & (1u << i)) == 0u) { continue; }
    let term = light_term(i, extractBits(c, 3u, 2u), extractBits(c, 5u, 2u), mv_pos, mv_nrm);
    lighting = lighting + i32(round(term * uf4(uoff(OFF_LIGHTS) + i * 5u + 2u).a * 255.0));
  }
  let lacc = clamp(lighting, 0, 255);
  let material = i32(round(mat.a * 255.0));
  return f32((material * (lacc + (lacc >> 7u))) >> 8u) / 255.0;
}

// --- Texture coordinate generation --------------------------------------------------------
fn texgen(i: u32, vidx: u32, pos: vec3f, nrm: vec3f, clr0: vec4f, clr1: vec4f) -> vec3f {
  let w = cfgw(W_TCGS + i);
  let kind = extractBits(w, 0u, 2u);
  let src = extractBits(w, 2u, 5u);
  let dual = (cfgw(W_GLOBAL0) & (1u << 14u)) != 0u;
  var tc: vec4f;
  if (src >= 4u && src <= 11u) {
    tc = vec4f(load_tex(src - 4u, vidx), 1.0, 1.0);
  } else if (src == 0u) {
    tc = vec4f(pos, 1.0);
  } else if (src == 1u) {
    tc = vec4f(nrm, 1.0);
  } else if (src == 2u) {
    tc = vec4f(load_nrm_group(vidx, 1u), 1.0);
  } else if (src == 3u) {
    tc = vec4f(load_nrm_group(vidx, 2u), 1.0);
  } else if (src == 12u) {
    tc = vec4f(clr0.rgb, 1.0);
  } else {
    tc = vec4f(clr1.rgb, 1.0);
  }
  if ((w & (1u << 21u)) != 0u) { tc.z = 1.0; }
  var tmp: vec3f;
  if (kind == 2u) {
    tmp = vec3f(tc.xy, 1.0);
  } else {
    let mode = extractBits(w, 7u, 2u);
    if (mode == 2u) {
      let loc = attr_loc(A_TEX0MTXIDX + i, vidx, 0u);
      tmp = tc * postex(ld8(loc.arr, loc.off) / 3u);
    } else if (mode == 1u) {
      tmp = tc * postex(extractBits(w, 9u, 5u));
    } else {
      tmp = tc.xyz;
    }
    if (kind == 1u) { tmp.z = 1.0; }
  }
  if (dual && (w & (1u << 20u)) != 0u) { tmp = normalize(tmp); }
  var p = tmp;
  if (dual && (w & (1u << 14u)) == 0u) {
    p = vec4f(tmp, 1.0) * mtx34(uoff(OFF_POSTMTX) + 3u * extractBits(w, 15u, 5u));
  }
  if (kind != 2u && p.z == 0.0) {
    p.x = clamp(p.x / 2.0, -1.0, 1.0);
    p.y = clamp(p.y / 2.0, -1.0, 1.0);
  }
  return p;
}

struct VertexOutput {
  @builtin(position) pos: vec4f,
  @location(0) cc0: vec4f,
  @location(1) cc1: vec4f,
  @location(2) tex0: vec3f,
  @location(3) tex1: vec3f,
  @location(4) tex2: vec3f,
  @location(5) tex3: vec3f,
  @location(6) tex4: vec3f,
  @location(7) tex5: vec3f,
  @location(8) tex6: vec3f,
  @location(9) tex7: vec3f,
};

@vertex
fn vs_main(@builtin(vertex_index) vidx: u32) -> VertexOutput {
  var out: VertexOutput;
  var pnmtx = ubuf[0u].y;
  if (attr_present(A_PNMTXIDX)) {
    let loc = attr_loc(A_PNMTXIDX, vidx, 0u);
    pnmtx = ld8(loc.arr, loc.off) / 3u;
  }
  let pos = load_pos(vidx);
  let mv_pos = vec4f(pos, 1.0) * postex(pnmtx);
  out.pos = vec4f(mv_pos, 1.0) * proj_mtx();
  // GX rasterizes at a 7/12 pixel center when antialiasing is disabled, while WebGPU rasterizes at 1/2.
  let render_size = bitcast<vec2f>(ubuf[0u].zw);
  let center_fix = vec2f(-1.0, 1.0) / (6.0 * max(abs(render_size), vec2f(1.0)));
  out.pos = vec4f(out.pos.xy + out.pos.w * center_fix, out.pos.zw);
  let nrm = load_nrm_group(vidx, 0u);
  let nrm_tmp = vec4f(nrm, 0.0) * nrm_mtx(pnmtx);
  let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);
  let clr0 = load_clr(0u, vidx);
  let clr1 = load_clr(1u, vidx);
  let sampled_channels = extractBits(cfgw(W_COLOR_SRC), 4u, 2u);
  if ((sampled_channels & 1u) != 0u) {
    let vcol = vertex_color(0u, clr0, clr1);
    out.cc0 = vec4f(lit_rgb(0u, mv_pos, mv_nrm, vcol), lit_alpha(0u, mv_pos, mv_nrm, vcol));
  }
  if ((sampled_channels & 2u) != 0u) {
    let vcol = vertex_color(1u, clr0, clr1);
    out.cc1 = vec4f(lit_rgb(1u, mv_pos, mv_nrm, vcol), lit_alpha(1u, mv_pos, mv_nrm, vcol));
  }
  var tcs: array<vec3f, 8>;
  let num_tex_gens = extractBits(cfgw(W_GLOBAL0), 5u, 4u);
  for (var i = 0u; i < num_tex_gens; i++) {
    if ((cfgw(W_TCGS + i) & (1u << 22u)) != 0u) {
      tcs[i] = texgen(i, vidx, pos, nrm, clr0, clr1);
    }
  }
  out.tex0 = tcs[0];
  out.tex1 = tcs[1];
  out.tex2 = tcs[2];
  out.tex3 = tcs[3];
  out.tex4 = tcs[4];
  out.tex5 = tcs[5];
  out.tex6 = tcs[6];
  out.tex7 = tcs[7];
  return out;
}

// --- TEV ------------------------------------------------------------------------------------
fn tev_overflow_f32(in: f32) -> f32 {
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}
fn tev_overflow_vec3f(in: vec3f) -> vec3f {
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}
fn tev_overflow_vec4f(in: vec4f) -> vec4f {
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}
fn tev_s24_fixed_vec2i(in: vec2i) -> vec2i { return (in << vec2u(8u)) >> vec2u(8u); }
fn tev_byte_f32(in: f32) -> i32 { return i32(round(in * 255.0)); }
fn tev_byte_vec3f(in: vec3f) -> vec3i { return vec3i(round(in * 255.0)); }
fn tev_regular_i32(a: i32, b: i32, c: i32, d: i32, bias: i32, scale: u32, is_sub: bool) -> i32 {
  var d_part = d + bias;
  var lerp_part = (a << 8u) + (b - a) * (c + (c >> 7u));
  if (scale == 1u) {
    d_part = d_part << 1u;
    lerp_part = lerp_part << 1u;
  }
  if (scale == 2u) {
    d_part = d_part << 2u;
    lerp_part = lerp_part << 2u;
  }
  if (scale != 3u) {
    lerp_part += select(128, 127, is_sub);
  }
  let lerp = lerp_part >> 8u;
  var result = select(d_part + lerp, d_part - lerp, is_sub);
  if (scale == 3u) {
    result = result >> 1u;
  }
  return result;
}
fn tev_regular_f32(a: f32, b: f32, c: f32, d: f32, bias: i32, scale: u32, is_sub: bool) -> f32 {
  return f32(tev_regular_i32(tev_byte_f32(a), tev_byte_f32(b), tev_byte_f32(c), tev_byte_f32(d), bias, scale, is_sub)) /
         255.0;
}
fn tev_regular_vec3f(a: vec3f, b: vec3f, c: vec3f, d: vec3f, bias: i32, scale: u32, is_sub: bool) -> vec3f {
  let ai = tev_byte_vec3f(a);
  let bi = tev_byte_vec3f(b);
  let ci = tev_byte_vec3f(c);
  let di = tev_byte_vec3f(d);
  return vec3f(
    f32(tev_regular_i32(ai.x, bi.x, ci.x, di.x, bias, scale, is_sub)) / 255.0,
    f32(tev_regular_i32(ai.y, bi.y, ci.y, di.y, bias, scale, is_sub)) / 255.0,
    f32(tev_regular_i32(ai.z, bi.z, ci.z, di.z, bias, scale, is_sub)) / 255.0,
  );
}
fn tev_bias(code: u32) -> i32 {
  if (code == 1u) { return 128; }
  if (code == 2u) { return -128; }
  return 0;
}

fn swz(v: vec4f, table: u32) -> vec4f {
  let t = extractBits(cfgw(W_SWAP), table * 8u, 8u);
  return vec4f(v[t & 3u], v[(t >> 2u) & 3u], v[(t >> 4u) & 3u], v[(t >> 6u) & 3u]);
}

fn konst_color(sel: u32) -> vec3f {
  if (sel < 8u) { return vec3f(f32(8u - sel) / 8.0); }
  if (sel >= 0x0Cu && sel <= 0x0Fu) { return uf4_opt(OFF_KCOLOR + sel - 0x0Cu).rgb; }
  return vec3f(uf4_opt(OFF_KCOLOR + (sel & 3u))[(sel - 0x10u) >> 2u]);
}
fn konst_alpha(sel: u32) -> f32 {
  if (sel < 8u) { return f32(8u - sel) / 8.0; }
  return uf4_opt(OFF_KCOLOR + (sel & 3u))[(sel - 0x10u) >> 2u];
}

fn color_arg(arg: u32, regs: ptr<function, array<vec4f, 4>>, tex: vec4f, ras: vec4f, k: vec3f) -> vec3f {
  switch arg {
    case 0u: { return (*regs)[0].rgb; }
    case 1u: { return vec3f((*regs)[0].a); }
    case 2u: { return (*regs)[1].rgb; }
    case 3u: { return vec3f((*regs)[1].a); }
    case 4u: { return (*regs)[2].rgb; }
    case 5u: { return vec3f((*regs)[2].a); }
    case 6u: { return (*regs)[3].rgb; }
    case 7u: { return vec3f((*regs)[3].a); }
    case 8u: { return tex.rgb; }
    case 9u: { return vec3f(tex.a); }
    case 10u: { return ras.rgb; }
    case 11u: { return vec3f(ras.a); }
    case 12u: { return vec3f(1.0); }
    case 13u: { return vec3f(0.5); }
    case 14u: { return k; }
    default: { return vec3f(0.0); }
  }
}
fn alpha_arg(arg: u32, regs: ptr<function, array<vec4f, 4>>, tex: vec4f, ras: vec4f, k: f32) -> f32 {
  switch arg {
    case 0u: { return (*regs)[0].a; }
    case 1u: { return (*regs)[1].a; }
    case 2u: { return (*regs)[2].a; }
    case 3u: { return (*regs)[3].a; }
    case 4u: { return tex.a; }
    case 5u: { return ras.a; }
    case 6u: { return k; }
    default: { return 0.0; }
  }
}

fn sample_map(m: u32, uv: vec2f, bias: f32) -> vec4f {
  switch m {
    case 0u: { return textureSampleBias(tex0, tex0_samp, uv, bias); }
    case 1u: { return textureSampleBias(tex1, tex1_samp, uv, bias); }
    case 2u: { return textureSampleBias(tex2, tex2_samp, uv, bias); }
    case 3u: { return textureSampleBias(tex3, tex3_samp, uv, bias); }
    case 4u: { return textureSampleBias(tex4, tex4_samp, uv, bias); }
    case 5u: { return textureSampleBias(tex5, tex5_samp, uv, bias); }
    case 6u: { return textureSampleBias(tex6, tex6_samp, uv, bias); }
    default: { return textureSampleBias(tex7, tex7_samp, uv, bias); }
  }
}

fn tex_uv(tcs: ptr<function, array<vec3f, 8>>, i: u32) -> vec2f {
  let v = (*tcs)[i];
  if (extractBits(cfgw(W_TCGS + i), 0u, 2u) == 0u && v.z != 0.0) { return v.xy / v.z; }
  return v.xy;
}
fn fixed_uv(tcs: ptr<function, array<vec3f, 8>>, i: u32) -> vec2i {
  return vec2i(tex_uv(tcs, i) * uf4_opt(OFF_TEXCOORD_SCALE + i).xy * 128.0);
}

fn wrap_comp(code: u32, v: i32) -> i32 {
  if (code == 0u) { return v; }
  if (code == 6u) { return 0; }
  return v & ((i32(256u >> (code - 1u)) << 7u) - 1);
}

fn alpha_test(comp: u32, v: u32, ref_value: u32) -> bool {
  switch comp {
    case 0u: { return false; }
    case 1u: { return v < ref_value; }
    case 2u: { return v == ref_value; }
    case 3u: { return v <= ref_value; }
    case 4u: { return v > ref_value; }
    case 5u: { return v != ref_value; }
    case 6u: { return v >= ref_value; }
    default: { return true; }
  }
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
  let g0 = cfgw(W_GLOBAL0);
  let stage_count = extractBits(g0, 0u, 5u);
  var tcs = array<vec3f, 8>(in.tex0, in.tex1, in.tex2, in.tex3, in.tex4, in.tex5, in.tex6, in.tex7);
  var regs: array<vec4f, 4>;
  for (var r = 0u; r < 4u; r++) { regs[r] = uf4_opt(OFF_TEV_REG + r); }

  // Indirect stages, each sampled once.
  var ind_coords: array<vec3i, 4>;
  for (var i = 0u; i < 4u; i++) {
    let w = cfgw(W_IND + i);
    if ((w & 1u) == 0u) { continue; }
    var fixed = vec2i(0);
    if ((w & 2u) != 0u) { fixed = fixed_uv(&tcs, extractBits(w, 2u, 3u)); }
    let m = extractBits(w, 5u, 3u);
    let size_bias = uf4_opt(OFF_TEX_SIZE + m);
    let sample_fixed = fixed >> vec2u(extractBits(w, 8u, 4u), extractBits(w, 12u, 4u));
    ind_coords[i] = tev_byte_vec3f(sample_map(m, vec2f(sample_fixed) / (size_bias.xy * 128.0), size_bias.z).abg);
  }

  let disabled_tex = select(0.0, 1.0, (g0 & (1u << 16u)) != 0u);
  var t_tex = vec2i(0);
  for (var s = 0u; s < stage_count; s++) {
    let w0 = cfgw(W_STAGES + s * 4u);
    let w1 = cfgw(W_STAGES + s * 4u + 1u);
    let w2 = cfgw(W_STAGES + s * 4u + 2u);
    let w3 = cfgw(W_STAGES + s * 4u + 3u);
    let will_sample = (w2 & 1u) != 0u;
    let tmap = extractBits(w2, 2u, 3u);
    let tcid = extractBits(w2, 6u, 3u);

    // Texture coordinate (indirect, wrapping) and sample.
    var uv = vec2f(0.0);
    var uv_ready = false;
    if ((w2 & (1u << 9u)) != 0u) {
      var offset = vec2i(0);
      var has_offset = false;
      if ((w3 & 1u) != 0u) {
        var ic = ind_coords[extractBits(w3, 1u, 2u)] >> vec3u(extractBits(w3, 3u, 3u));
        let bias_value = select(1, -128, (w3 & (1u << 9u)) != 0u);
        ic = ic + vec3i(i32(extractBits(w3, 6u, 1u)), i32(extractBits(w3, 7u, 1u)), i32(extractBits(w3, 8u, 1u))) * bias_value;
        let kind = extractBits(w3, 10u, 2u);
        let mi = extractBits(w3, 12u, 2u);
        let im = uoff(OFF_IND_MTX);
        if (kind == 1u) {
          let c0 = bitcast<vec4i>(ubuf[im + mi * 2u]);
          let c1 = bitcast<vec4i>(ubuf[im + mi * 2u + 1u]);
          offset = vec2i((c0.x * ic.x + c0.z * ic.y + c1.x * ic.z) >> 3u,
                         (c0.y * ic.x + c0.w * ic.y + c1.y * ic.z) >> 3u);
          has_offset = true;
        } else if (kind == 2u) {
          offset = (fixed_uv(&tcs, tcid) * vec2i(ic.x)) >> vec2u(8u);
          has_offset = true;
        } else if (kind == 3u) {
          offset = (fixed_uv(&tcs, tcid) * vec2i(ic.y)) >> vec2u(8u);
          has_offset = true;
        }
        if (has_offset) {
          let shift = bitcast<vec4i>(ubuf[im + mi * 2u + 1u]).z;
          if (shift >= 0) {
            offset = offset >> vec2u(u32(shift));
          } else {
            offset = offset << vec2u(u32(-shift));
          }
        }
      }
      var base = vec2i(0);
      if ((w2 & (1u << 17u)) != 0u) { base = fixed_uv(&tcs, tcid); }
      var coord = vec2i(wrap_comp(extractBits(w3, 14u, 3u), base.x), wrap_comp(extractBits(w3, 17u, 3u), base.y));
      if (has_offset) { coord = coord + offset; }
      if ((w3 & (1u << 20u)) != 0u) {
        t_tex = t_tex + coord;
      } else {
        t_tex = coord;
      }
      t_tex = tev_s24_fixed_vec2i(t_tex);
      if (will_sample) {
        uv = vec2f(t_tex) / (uf4_opt(OFF_TEX_SIZE + tmap).xy * 128.0);
        uv_ready = true;
      }
    }
    var sampled = vec4f(0.0);
    if (will_sample) {
      let size_bias = uf4_opt(OFF_TEX_SIZE + tmap);
      if (!uv_ready) { uv = vec2f(tev_s24_fixed_vec2i(fixed_uv(&tcs, tcid))) / (size_bias.xy * 128.0); }
      sampled = sample_map(tmap, uv, size_bias.z);
    }

    // Combiner inputs.
    var tex = swz(sampled, extractBits(w2, 15u, 2u));
    if ((w2 & 2u) == 0u) { tex = vec4f(disabled_tex); }
    var ras = vec4f(0.0);
    let ras_kind = extractBits(w2, 10u, 2u);
    if (ras_kind == 1u) {
      ras = swz(in.cc0, extractBits(w2, 13u, 2u));
    } else if (ras_kind == 2u) {
      ras = swz(in.cc1, extractBits(w2, 13u, 2u));
    } else if (ras_kind == 3u) {
      var bump = 0.0;
      let sel = extractBits(w2, 18u, 2u);
      if (sel != 0u) {
        let v = ind_coords[extractBits(w3, 1u, 2u)][sel - 1u];
        let mask_code = extractBits(w2, 20u, 2u);
        let mask = select(select(0xF8u, 0xF0u, mask_code == 2u), 0xE0u, mask_code == 1u);
        bump = f32(u32(v) & mask) / 255.0;
        if ((w2 & (1u << 12u)) != 0u) { bump = bump * (255.0 / 248.0); }
      }
      ras = vec4f(bump);
    }
    let kc = konst_color(extractBits(w1, 16u, 5u));
    let ka = konst_alpha(extractBits(w1, 21u, 5u));

    // Color, then alpha.
    let ca = color_arg(extractBits(w0, 0u, 4u), &regs, tex, ras, kc);
    let cb = color_arg(extractBits(w0, 4u, 4u), &regs, tex, ras, kc);
    let cc = color_arg(extractBits(w0, 8u, 4u), &regs, tex, ras, kc);
    let cd = color_arg(extractBits(w0, 12u, 4u), &regs, tex, ras, kc);
    let color = tev_regular_vec3f(tev_overflow_vec3f(ca), tev_overflow_vec3f(cb), tev_overflow_vec3f(cc), cd,
                                  tev_bias(extractBits(w1, 1u, 2u)), extractBits(w1, 3u, 2u), (w1 & 1u) != 0u);
    var color_out: vec3f;
    if ((w1 & (1u << 5u)) != 0u) {
      color_out = clamp(color, vec3f(0.0), vec3f(1.0));
    } else {
      color_out = clamp(color, vec3f(-1024.0 / 255.0), vec3f(1023.0 / 255.0));
    }
    let color_reg = extractBits(w1, 6u, 2u);
    regs[color_reg] = vec4f(color_out, regs[color_reg].a);

    let aa = alpha_arg(extractBits(w0, 16u, 3u), &regs, tex, ras, ka);
    let ab = alpha_arg(extractBits(w0, 19u, 3u), &regs, tex, ras, ka);
    let ac = alpha_arg(extractBits(w0, 22u, 3u), &regs, tex, ras, ka);
    let ad = alpha_arg(extractBits(w0, 25u, 3u), &regs, tex, ras, ka);
    let alpha = tev_regular_f32(tev_overflow_f32(aa), tev_overflow_f32(ab), tev_overflow_f32(ac), ad,
                                tev_bias(extractBits(w1, 9u, 2u)), extractBits(w1, 11u, 2u), (w1 & (1u << 8u)) != 0u);
    var alpha_out: f32;
    if ((w1 & (1u << 13u)) != 0u) {
      alpha_out = clamp(alpha, 0.0, 1.0);
    } else {
      alpha_out = clamp(alpha, -1024.0 / 255.0, 1023.0 / 255.0);
    }
    regs[extractBits(w1, 14u, 2u)].a = alpha_out;
  }

  let last = cfgw(W_STAGES + (stage_count - 1u) * 4u + 1u);
  if (extractBits(last, 6u, 2u) != 0u) { regs[0] = vec4f(regs[extractBits(last, 6u, 2u)].rgb, regs[0].a); }
  if (extractBits(last, 14u, 2u) != 0u) { regs[0].a = regs[extractBits(last, 14u, 2u)].a; }
  var prev = regs[0];

  // Fog.
  let fog_fn = extractBits(g0, 9u, 3u);
  if (fog_fn != 0u) {
    let fog = uoff(OFF_FOG);
    let fog_color = uf4(fog);
    let fog_params = uf4(fog + 1u); // a, b, c, shift
    let z_coord = u32(round(clamp(FOG_DEPTH, 0.0, 1.0) * 16777216.0));
    var ze: f32;
    if ((g0 & (1u << 12u)) == 0u) {
      let shifted = z_coord >> u32(fog_params.w);
      ze = (fog_params.x * 16777216.0) / (fog_params.y - f32(shifted));
    } else {
      ze = fog_params.x * f32(z_coord) / 16777216.0;
    }
    if ((g0 & (1u << 13u)) != 0u) {
      let range_base = uf4(fog + 2u);
      let range_offset = (2.0 * (in.pos.x / range_base.y)) - 1.0 - range_base.x;
      let float_index = clamp(9.0 - abs(range_offset) * 9.0, 0.0, 9.0);
      let lower = u32(float_index);
      let upper = lower + 1u;
      let k_lower = uf4(fog + 3u + (lower >> 2u))[lower & 3u];
      let k_upper = uf4(fog + 3u + (upper >> 2u))[upper & 3u];
      let k = mix(k_lower, k_upper, fract(float_index));
      ze *= sqrt(range_offset * range_offset + k * k) / k;
    }
    var f = clamp(ze - fog_params.z, 0.0, 1.0);
    var fz: f32;
    switch fog_fn {
      case 1u: { fz = f; }
      case 2u: { fz = 1.0 - exp2(-8.0 * f); }
      case 3u: { fz = 1.0 - exp2(-8.0 * f * f); }
      case 4u: { fz = exp2(-8.0 * (1.0 - f)); }
      default: {
        f = 1.0 - f;
        fz = exp2(-8.0 * f * f);
      }
    }
    prev = vec4f(mix(prev.rgb, fog_color.rgb, clamp(fz, 0.0, 1.0)), prev.a);
  }

  prev = tev_overflow_vec4f(prev);
  if (extractBits(g0, 17u, 2u) != 0u) {
    let aw = cfgw(W_ALPHA);
    let value = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));
    let p0 = alpha_test(extractBits(aw, 0u, 3u), value, extractBits(aw, 3u, 8u));
    let p1 = alpha_test(extractBits(aw, 13u, 3u), value, extractBits(aw, 16u, 8u));
    var pass_test: bool;
    switch extractBits(aw, 11u, 2u) {
      case 1u: { pass_test = p0 || p1; }
      case 2u: { pass_test = p0 != p1; }
      case 3u: { pass_test = p0 == p1; }
      default: { pass_test = p0 && p1; }
    }
    if (!pass_test) { discard; }
  }
  return prev;
}
)WGSL";

std::string ubershader_source() {
  std::string constants = fmt::format(
      "const W_GLOBAL0: u32 = {}u;\nconst W_ALPHA: u32 = {}u;\nconst W_SWAP: u32 = {}u;\nconst W_CHANNELS: u32 = {}u;\n"
      "const W_COLOR_SRC: u32 = {}u;\nconst W_GLOBAL1: u32 = {}u;\nconst W_OFFSETS: u32 = {}u;\n"
      "const W_ATTRS: u32 = {}u;\nconst W_TCGS: u32 = {}u;\nconst W_IND: u32 = {}u;\nconst W_STAGES: u32 = {}u;\n"
      "const OFF_POSTEX: u32 = {}u;\nconst OFF_NRM: u32 = {}u;\nconst OFF_TEV_REG: u32 = {}u;\n"
      "const OFF_LIGHTS: u32 = {}u;\nconst OFF_LIGHT_STATE: u32 = {}u;\nconst OFF_CHANNEL_COLOR: u32 = {}u;\n"
      "const OFF_KCOLOR: u32 = {}u;\nconst OFF_POSTMTX: u32 = {}u;\nconst OFF_FOG: u32 = {}u;\n"
      "const OFF_IND_MTX: u32 = {}u;\nconst OFF_TEXCOORD_SCALE: u32 = {}u;\nconst OFF_TEX_SIZE: u32 = {}u;\n"
      "const A_PNMTXIDX: u32 = {}u;\nconst A_TEX0MTXIDX: u32 = {}u;\nconst A_POS: u32 = {}u;\nconst A_NRM: u32 = {}u;\n"
      "const A_CLR0: u32 = {}u;\nconst A_TEX0: u32 = {}u;\n",
      kWordGlobal0, kWordAlpha, kWordSwap, kWordChannels, kWordColorSrc, kWordGlobal1, kWordOffsets, kWordAttrs,
      kWordTcgs, kWordInd, kWordStages, underlying(kOffPostex), underlying(kOffNrm), underlying(kOffTevReg),
      underlying(kOffLights), underlying(kOffLightState), underlying(kOffChannelColor), underlying(kOffKColor),
      underlying(kOffPostMtx), underlying(kOffFog), underlying(kOffIndMtx), underlying(kOffTexCoordScale),
      underlying(kOffTexSize), static_cast<u32>(GX_VA_PNMTXIDX), static_cast<u32>(GX_VA_TEX0MTXIDX),
      static_cast<u32>(GX_VA_POS), static_cast<u32>(GX_VA_NRM), static_cast<u32>(GX_VA_CLR0),
      static_cast<u32>(GX_VA_TEX0));
  std::string body{kUberShaderBody};
  const auto replace = [&](std::string_view token, std::string_view value) {
    for (size_t at = body.find(token); at != std::string::npos; at = body.find(token, at + value.size())) {
      body.replace(at, token.size(), value);
    }
  };
  replace("UNIFORM_VEC4S", fmt::format("{}", MaxUniformSize / 16));
  replace("FOG_DEPTH", UseReversedZ ? "(1.0 - in.pos.z)" : "in.pos.z");
  return constants + body;
}
} // namespace

wgpu::ShaderModule ubershader_module() noexcept {
  static std::once_flag once;
  static wgpu::ShaderModule module;
  std::call_once(once, [] {
    const std::string source = ubershader_source();
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = source.c_str();
    const wgpu::ShaderModuleDescriptor descriptor{
        .nextInChain = &wgsl,
        .label = "GX Ubershader",
    };
    module = webgpu::g_device.CreateShaderModule(&descriptor);
    Log.info("GX ubershader created ({} bytes of WGSL)", source.size());
  });
  return module;
}

} // namespace aurora::gx
