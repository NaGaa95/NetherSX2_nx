#include "dk_native.h"
#include "../saltynx.h"
#include "dk_vertex_format.h"
#include "dk_utility_target.h"
#include "dk_render_state.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

extern "C" {
#include "../config.h"
#include "../error.h"
#include "../prefs.h"
}

namespace nxdk {
namespace {
std::string read_resource(std::string_view path) {
  const std::string filename = std::string(RESOURCES_DIR "/") + std::string(path);
  FILE *file = fopen(filename.c_str(), "rb");
  if (!file) throw std::runtime_error("Missing shader resource: " + filename);
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  rewind(file);
  std::string contents;
  if (size > 0 && size <= 4 * 1024 * 1024) contents.resize(size);
  const bool read = !contents.empty() && fread(contents.data(), 1, contents.size(), file) == contents.size();
  fclose(file);
  if (!read) throw std::runtime_error("Could not read shader resource: " + filename);
  return contents;
}
void define(std::string &source, std::string_view name, std::string_view value) {
  const std::string key = "#define " + std::string(name) + " ";
  const size_t offset = source.find(key);
  if (offset == source.npos) source.insert(source.find('\n') + 1, key + std::string(value) + "\n");
  else source.replace(offset, source.find('\n', offset) - offset, key + std::string(value));
}

struct NativeState {
  uint64_t generation = 0;
  const DkImage *color = nullptr, *depth = nullptr;
  DkViewport viewport{};
  DkScissor scissor{};
  DkColorState color_state{};
  DkColorWriteState write_state{};
  DkBlendState blend{};
  DkDepthStencilState depth_state{};
  std::array<const DkShader *, 3> shaders{};
  DkResHandle textures[6][16]{};
  uint16_t texture_valid[6]{};
  struct Uniform { DkGpuAddr address = 0; uint32_t size = 0; } uniforms[6][16];
  bool shaders_valid = false, raster_valid = false, vertex_valid = false, hardware_vertices = false;
  bool descriptors_valid = false;
  bool target_valid = false, state_valid = false, viewport_valid = false, scissor_valid = false;
};
NativeState state;
void refresh_state() {
  if (state.generation != gpu().generation()) {
    state = {};
    state.generation = gpu().generation();
  }
}
void bind_uniform(DkStage stage, unsigned slot, DkGpuAddr address, uint32_t size) {
  refresh_state();
  auto &previous = state.uniforms[stage][slot];
  if (previous.address != address || previous.size != size) {
    dkCmdBufBindUniformBuffer(gpu().commands(), stage, slot, address, size);
    previous = {address, size};
  }
}
void bind_descriptors() {
  refresh_state();
  if (!state.descriptors_valid) {
    gpu().bind_descriptor_sets();
    state.descriptors_valid = true;
  }
}
struct Device {
  void *core = nullptr;
  std::string common, tfx_vertex;
  std::map<std::string, KernelPtr> utility;
  std::map<std::array<uint32_t, 4>, KernelPtr> pixels;
  std::unordered_map<uint64_t, KernelPtr> vertices;
  std::array<KernelPtr, 16> geometry;
  NativeTexture *dummy = nullptr;
  unsigned anisotropy = 1;
  std::vector<NativeTexture *> transient;
  uint64_t uniform_generation = 0;
  std::array<uint8_t, 48> vs_constants{};
  std::array<uint8_t, 208> ps_constants{};
  DkGpuAddr vs_uniform = 0, ps_uniform = 0;
  bool vs_valid = false, ps_valid = false;
};
Device device;

const char *convert_name(unsigned index) {
  static constexpr const char *common[] = {
      "ps_copy", "ps_convert_rgba8_16bits", "ps_datm1", "ps_datm0", "ps_hdr_init",
      "ps_hdr_resolve", "ps_filter_transparency", "ps_convert_float32_32bits",
      "ps_convert_float32_32bits", "ps_convert_float32_rgba8", "ps_convert_float16_rgb5a1",
      "ps_convert_rgba8_float32", "ps_convert_rgba8_float24", "ps_convert_rgba8_float16",
      "ps_convert_rgb5a1_float16", "ps_convert_rgba8_float32_biln", "ps_convert_rgba8_float24_biln",
      "ps_convert_rgba8_float16_biln", "ps_convert_rgb5a1_float16_biln", "ps_depth_copy",
      "ps_convert_rgba_8i"};
  if (index < std::size(common)) return common[index];
  if (profile->device_method_count == 32) return index == 21 ? "ps_yuv" : nullptr;
  return index == 21 ? "ps_convert_clut_4" : index == 22 ? "ps_convert_clut_8" :
         index == 23 ? "ps_yuv" : nullptr;
}
KernelPtr convert_kernel(unsigned index) {
  const char *name = convert_name(index);
  if (!name) fatal_error("Deko3D: unknown conversion shader %u", index);
  const float upscale = *reinterpret_cast<const float *>(core_address(profile->upscale_address));
  char scale[48];
  snprintf(scale, sizeof(scale), "%.8f", double(upscale));
  return utility_kernel("shaders/opengl/convert.glsl", name, DkStage_Fragment,
                        std::string("#define PS_SCALE_FACTOR ") + scale + "\n");
}
DkSampler point_sampler(bool linear = false) {
  DkSampler sampler;
  dkSamplerDefaults(&sampler);
  sampler.minFilter = sampler.magFilter = linear ? DkFilter_Linear : DkFilter_Nearest;
  for (auto &wrap : sampler.wrapMode) wrap = DkWrapMode_ClampToEdge;
  return sampler;
}
void native_state(uint8_t mask, const DkDepthStencilState &depth, bool blend,
                  DkBlendFactor source = DkBlendFactor_SrcAlpha,
                  DkBlendFactor destination = DkBlendFactor_InvSrcAlpha,
                  DkBlendOp operation = DkBlendOp_Add) {
  refresh_state();
  DkColorState color{};
  dkColorStateDefaults(&color);
  color.blendEnableMask = blend ? 1 : 0;
  DkColorWriteState write{};
  dkColorWriteStateDefaults(&write);
  dkColorWriteStateSetMask(&write, 0, mask & 15);
  const DkBlendState blend_state = gs_blend_state(source, destination, operation);
  DkCmdBuf commands = gpu().commands();
  if (!state.state_valid || memcmp(&state.color_state, &color, sizeof(color)))
    dkCmdBufBindColorState(commands, &color);
  if (!state.state_valid || memcmp(&state.write_state, &write, sizeof(write)))
    dkCmdBufBindColorWriteState(commands, &write);
  if (!state.state_valid || memcmp(&state.blend, &blend_state, sizeof(blend_state)))
    dkCmdBufBindBlendState(commands, 0, &blend_state);
  if (!state.state_valid || memcmp(&state.depth_state, &depth, sizeof(depth)))
    dkCmdBufBindDepthStencilState(commands, &depth);
  state.color_state = color;
  state.write_state = write;
  state.blend = blend_state;
  state.depth_state = depth;
  state.state_valid = true;
}
DkDepthStencilState depth_state(uint8_t selector, bool has_depth) {
  DkDepthStencilState result{};
  dkDepthStencilStateDefaults(&result);
  const unsigned test = selector & 3;
  static constexpr DkCompareOp tests[]{DkCompareOp_Never, DkCompareOp_Always,
                                     DkCompareOp_Gequal, DkCompareOp_Greater};
  result.depthTestEnable = has_depth;
  result.depthWriteEnable = has_depth && (selector & 4);
  result.depthCompareOp = tests[test];
  result.stencilTestEnable = has_depth && (selector & 8);
  result.stencilFrontCompareOp = result.stencilBackCompareOp = DkCompareOp_Equal;
  result.stencilFrontFailOp = result.stencilBackFailOp = DkStencilOp_Keep;
  result.stencilFrontDepthFailOp = result.stencilBackDepthFailOp = DkStencilOp_Keep;
  result.stencilFrontPassOp = result.stencilBackPassOp = selector & 16 ? DkStencilOp_Zero : DkStencilOp_Keep;
  return result;
}

void vertex_format(bool hardware) {
  refresh_state();
  if (state.vertex_valid && state.hardware_vertices == hardware) return;
  static constexpr auto hardware_attributes = gs_vertex_attributes(true);
  static constexpr auto utility_attributes = gs_vertex_attributes(false);
  const auto &attributes = hardware ? hardware_attributes : utility_attributes;
  const DkVtxBufferState buffer{32, 0};
  dkCmdBufBindVtxAttribState(gpu().commands(), attributes.data(), attributes.size());
  dkCmdBufBindVtxBufferState(gpu().commands(), &buffer, 1);
  state.vertex_valid = true;
  state.hardware_vertices = hardware;
}
DkGpuAddr upload_constants(const void *data, unsigned bytes) {
  const uint32_t size = align_up(bytes, DK_UNIFORM_BUF_ALIGNMENT);
  const Upload contents = gpu().upload(size);
  if (!contents.cpu) fatal_error("Deko3D: out of uniform memory");
  memset(contents.cpu, 0, size);
  memcpy(contents.cpu, data, bytes);
  return contents.gpu;
}
}

void invalidate_native_state() { state = {}; }

void Kernel::uniform(std::string_view name, const void *data, size_t size) {
  for (const auto &uniform : source.uniforms)
    if (uniform.name == name && uniform.sampler < 0) {
      const size_t bytes = std::min<size_t>(size, uniform.size);
      if (memcmp(uniforms.data() + uniform.offset, data, bytes)) {
        memcpy(uniforms.data() + uniform.offset, data, bytes);
        uniform_gpu = 0;
      }
      return;
    }
}

KernelPtr make_kernel(std::string source, DkStage stage) {
  auto result = std::make_shared<Kernel>();
  result->hdr = shader_define(source, "PS_HDR") != 0;
  result->date = shader_define(source, "PS_DATE");
  result->source = lower_shader(source, stage == DkStage_Vertex);
  result->uniforms.resize(result->source.uniform_bytes);
  std::string diagnostics;
  result->binary = compile_shader(result->source, stage, diagnostics);
  if (!result->binary) fatal_error("Deko3D shader failed:\n%s", diagnostics.c_str());
  return result;
}

KernelPtr utility_kernel(std::string_view file, std::string_view entry, DkStage stage,
                         std::string_view defines) {
  const std::string key = std::string(file) + "|" + std::string(entry) + "|" +
                          std::to_string(stage) + "|" + std::string(defines);
  if (auto found = device.utility.find(key); found != device.utility.end()) return found->second;
  std::string source = read_resource(file);

  for (const char *include : {"ffx_a.h", "ffx_cas.h"}) {
    const std::string directive = std::string("#include \"") + include + "\"";
    const size_t position = source.find(directive);
    if (position != source.npos)
      source.replace(position, directive.size(), read_resource(std::string("shaders/common/") + include));
  }
  std::string header = "#version 450 core\n#define pGL_ES 1\n#define HAS_CLIP_CONTROL 1\n"
                       "#define HAS_FRAMEBUFFER_FETCH 0\n";
  if (stage == DkStage_Vertex) header += "#define VERTEX_SHADER 1\n";
  if (stage == DkStage_Fragment) header += "#define FRAGMENT_SHADER 1\n";
  if (stage == DkStage_Compute) header += "#define COMPUTE_SHADER 1\n";
  if (entry != "main") header += "#define " + std::string(entry) + " main\n";
  if (entry.starts_with("ps_stencil_image_init")) header += "#define " + std::string(entry) + " 1\n";

  if (entry.starts_with("ps_stencil_image_init")) {
    const std::string alias = "#define " + std::string(entry) + " main\n";
    header.erase(header.find(alias), alias.size());
  }
  header += defines;
  auto kernel = make_kernel(header + (stage == DkStage_Compute ? "" : device.common) + source, stage);
  device.utility.emplace(key, kernel);
  return kernel;
}

void bind_native_shaders(const KernelPtr &vertex, const KernelPtr &pixel, const KernelPtr &geometry) {
  refresh_state();
  std::array<const DkShader *, 3> shaders{};
  unsigned count = 0;
  if (vertex) shaders[count++] = &vertex->binary->shader;
  if (pixel) shaders[count++] = &pixel->binary->shader;
  if (geometry) shaders[count++] = &geometry->binary->shader;
  if (!state.shaders_valid || state.shaders != shaders) {
    dkCmdBufBindShaders(gpu().commands(), DkStageFlag_GraphicsMask, shaders.data(), count);
    state.shaders = shaders;
    state.shaders_valid = true;
  }
  if (!state.raster_valid) {
    DkRasterizerState raster{};
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = DkFace_None;
    raster.provokingVertex = GsProvokingVertex;
    dkCmdBufBindRasterizerState(gpu().commands(), &raster);
    state.raster_valid = true;
  }
}

void bind_kernel_uniforms(Kernel &kernel, DkStage stage) {
  if (kernel.uniforms.empty()) return;
  if (!kernel.uniform_gpu || kernel.uniform_generation != gpu().generation()) {
    kernel.uniform_gpu = upload_constants(kernel.uniforms.data(), kernel.uniforms.size());
    kernel.uniform_generation = gpu().generation();
  }
  bind_uniform(stage, LooseUniformBinding, kernel.uniform_gpu,
                align_up(kernel.uniforms.size(), DK_UNIFORM_BUF_ALIGNMENT));
}

void bind_native_texture(const NativeTexture *value, const DkSampler &sampler, unsigned slot,
                         DkStage stage) {
  if (!value) value = device.dummy;
  DkImageView view;
  dkImageViewDefaults(&view, &value->image->image);
  const DkResHandle handle = gpu().descriptor(view, sampler);
  refresh_state();
  if (!(state.texture_valid[stage] & (1u << slot)) || state.textures[stage][slot] != handle) {
    dkCmdBufBindTexture(gpu().commands(), stage, slot, handle);
    state.textures[stage][slot] = handle;
    state.texture_valid[stage] |= 1u << slot;
  }
}

void bind_target(Image *color, Image *depth, const Rect *requested) {
  refresh_state();
  Image *size = color ? color : depth;
  if (!size) return;
  const DkImage *color_image = color ? &color->image : nullptr;
  const DkImage *depth_image = depth ? &depth->image : nullptr;
  if (!state.target_valid || state.color != color_image || state.depth != depth_image) {
    if ((color && color->is_depth()) || (depth && !depth->is_depth()))
      fatal_error("Deko3D: invalid render target format.");
    DkImageView color_view{}, depth_view{};
    if (color) color_view = color->view();
    if (depth) depth_view = depth->view();
    bind_render_targets(gpu().commands(), color ? &color_view : nullptr,
                        depth ? &depth_view : nullptr);
    state.color = color_image;
    state.depth = depth_image;
    state.target_valid = true;
  }
  const DkViewport viewport{0, 0, float(size->width), float(size->height), 0.0f, 1.0f};
  const Rect rect = requested ? *requested : Rect{0, 0, int(size->width), int(size->height)};
  const int x = std::clamp(rect.x, 0, int(size->width)), y = std::clamp(rect.y, 0, int(size->height));
  const DkScissor scissor{uint32_t(x), uint32_t(y),
      uint32_t(std::max(0, std::min(rect.z, int(size->width)) - x)),
      uint32_t(std::max(0, std::min(rect.w, int(size->height)) - y))};
  if (!state.viewport_valid || memcmp(&state.viewport, &viewport, sizeof(viewport)))
    dkCmdBufSetViewports(gpu().commands(), 0, &viewport, 1);
  if (!state.scissor_valid || memcmp(&state.scissor, &scissor, sizeof(scissor)))
    dkCmdBufSetScissors(gpu().commands(), 0, &scissor, 1);
  state.viewport = viewport;
  state.scissor = scissor;
  state.viewport_valid = state.scissor_valid = true;
}

void clear_color(NativeTexture *target, const float *color) {
  if (!target || !color) return;
  bind_target(target->image.get(), nullptr);
  if (target->abi.format == 5 || target->abi.format == 6) {
    uint32_t integer[4];
    for (unsigned i = 0; i < 4; ++i) integer[i] = uint32_t(color[i]);
    dkCmdBufClearColor(gpu().commands(), 0, DkColorMask_RGBA, integer);
  } else dkCmdBufClearColor(gpu().commands(), 0, DkColorMask_RGBA, color);
  gpu().image_written(target->image.get());
  target->abi.state = 1;
}
void clear_depth(NativeTexture *target) {
  if (!target) return;
  bind_target(nullptr, target->image.get());
  dkCmdBufClearDepthStencil(gpu().commands(), true, 0.0f, 0, 0);
  gpu().image_written(target->image.get());
  target->abi.state = 1;
}
void clear_stencil(NativeTexture *target, uint8_t value) {
  if (!target) return;
  bind_target(nullptr, target->image.get());
  dkCmdBufClearDepthStencil(gpu().commands(), false, 0.0f, 0xff, value);
  gpu().image_written(target->image.get());
}
void copy_texture(NativeTexture *source, NativeTexture *destination, const Rect &rect,
                  uint32_t x, uint32_t y) {
  if (!source || !destination || rect.x < 0 || rect.y < 0 || rect.z <= rect.x || rect.w <= rect.y ||
      rect.z > source->abi.width || rect.w > source->abi.height ||
      uint64_t(x) + rect.z - rect.x > destination->image->width ||
      uint64_t(y) + rect.w - rect.y > destination->image->height) return;
  const DkImageView src = source->image->view(), dst = destination->image->view();
  const DkImageRect src_rect{uint32_t(rect.x), uint32_t(rect.y), 0,
                           uint32_t(rect.z - rect.x), uint32_t(rect.w - rect.y), 1};
  const DkImageRect dst_rect{x, y, 0, src_rect.width, src_rect.height, 1};
  gpu().barrier(DkBarrier_Full, 0);
  dkCmdBufCopyImage(gpu().commands(), &src, &src_rect, &dst, &dst_rect, 0);
  gpu().barrier(DkBarrier_Full, DkInvalidateFlags_Image);
  destination->abi.state = 0;
}

void draw_rect(NativeTexture *source, const FloatRect &sr, Image *target, const FloatRect &dr,
               KernelPtr pixel, bool linear, uint8_t mask, bool blend, Image *depth, bool stencil) {
  if (!source || (!target && !depth) || !pixel) return;
  const auto attachments = utility_targets(target, depth);
  Image *size = attachments.color ? attachments.color : attachments.depth;
  gpu().reserve_descriptors(1);
  bind_target(attachments.color, attachments.depth);
  const auto vertex = utility_kernel("shaders/opengl/convert.glsl", "vs_main", DkStage_Vertex);
  bind_native_shaders(vertex, pixel);
  const auto dss = attachments.depth_stencil(stencil);
  if (stencil) {
    dkCmdBufSetStencil(gpu().commands(), DkFace_FrontAndBack, 1, 1, 1);
  }
  native_state(attachments.color_mask(mask), dss, attachments.color && blend);
  vertex_format(false);
  struct Vertex { float position[4], uv[2]; uint32_t pad, color; };
  const float x0 = dr.x * 2 / size->width - 1, x1 = dr.z * 2 / size->width - 1;
  const float y0 = dr.y * 2 / size->height - 1, y1 = dr.w * 2 / size->height - 1;
  const Vertex vertices[] = {{{x0,y0,0,0},{sr.x,sr.y},0,0xffffffff},
                             {{x1,y0,0,0},{sr.z,sr.y},0,0xffffffff},
                             {{x0,y1,0,0},{sr.x,sr.w},0,0xffffffff},
                             {{x1,y1,0,0},{sr.z,sr.w},0,0xffffffff}};
  const Upload upload = gpu().upload(sizeof(vertices), 16);
  if (!upload.cpu) fatal_error("Deko3D: no vertex upload memory");
  memcpy(upload.cpu, vertices, sizeof(vertices));
  dkCmdBufBindVtxBuffer(gpu().commands(), 0, upload.gpu, upload.size);
  bind_descriptors();
  bind_native_texture(source, point_sampler(linear), 0);
  bind_kernel_uniforms(*pixel, DkStage_Fragment);
  gpu().prepare_sampled_image(source->image.get());
  dkCmdBufDraw(gpu().commands(), DkPrimitive_TriangleStrip, 4, 1, 0, 0);
  gpu().image_written(attachments.color);
  if (dss.depthWriteEnable || dss.stencilTestEnable) gpu().image_written(attachments.depth);
}

bool create_native_device(void *core) {
  try {
    if (!gpu().initialize()) return false;
    native_device_shutdown();

    device.core = core;
    device.anisotropy = std::clamp(prefs_get_int("EmuCore/GS/MaxAnisotropy", 1), 1, 16);
    device.common = read_resource("shaders/opengl/common_header.glsl");
    device.tfx_vertex = read_resource("shaders/opengl/tfx_vgs.glsl");
    auto *strings = reinterpret_cast<CoreString *>(static_cast<uint8_t *>(core) + profile->common_header_offset);
    strings[0].assign(device.common);
    strings[1].assign(device.tfx_vertex);
    strings[2].assign(read_resource("shaders/opengl/tfx_fs.glsl"));
    const uint32_t features = native_core_features(profile->feature_bytes);
    memcpy(static_cast<uint8_t *>(core) + profile->feature_offset, &features, profile->feature_bytes);
    device.dummy = create_texture(3, 1, 1, 1, 1);
    if (!device.dummy) return false;
    const uint32_t black = 0;
    if (!update_texture(device.dummy, {0,0,1,1}, &black, sizeof(black))) return false;

    utility_kernel("shaders/opengl/convert.glsl", "vs_main", DkStage_Vertex);
    convert_kernel(0);

    lsfg_prepare();

    return true;
  } catch (const std::exception &) {
    return false;
  }
}

void destroy_native_device(void *core) {
  gpu().finish();
  native_device_shutdown();
  auto *strings = reinterpret_cast<CoreString *>(static_cast<uint8_t *>(core) + profile->common_header_offset);
  for (unsigned i = 0; i < 3; ++i) strings[i].release();
  reinterpret_cast<void (*)(void *)>(core_address(profile->base_destructor))(core);
}

void native_device_shutdown() {
  if (!gpu().device()) return;

  if (!device.core && !device.dummy && device.transient.empty()) return;
  gpu().finish();
  for (NativeTexture *value : device.transient) destroy_texture(value);
  destroy_texture(device.dummy);
  device = {};
  invalidate_native_state();
}

namespace {
NativeTexture *temporary_texture(unsigned width, unsigned height, uint8_t format) {
  for (auto *image : device.transient)
    if (image->image->width == width && image->image->height == height && image->abi.format == format)
      return image;
  if (device.transient.size() >= 16) {
    destroy_texture(device.transient.front());
    device.transient.erase(device.transient.begin());
  }
  auto *image = create_texture(1, width, height, 1, format);
  if (!image) fatal_error("Deko3D: out of temporary image memory (%ux%u)", width, height);
  device.transient.push_back(image);
  return image;
}

KernelPtr pixel_kernel(const PsSelector &selector, int date = -1, bool alpha_only = false) {
  const std::array<uint32_t, 4> key{selector.words[0], selector.words[1], selector.words[2],
      uint32_t(date + 1) | (alpha_only ? 0x100 : 0)};
  if (auto found = device.pixels.find(key); found != device.pixels.end()) return found->second;
  std::string source = core_pixel_source(device.core, selector);
  define(source, "pGL_ES", "1");
  define(source, "HAS_CLIP_CONTROL", "1");
  define(source, "HAS_FRAMEBUFFER_FETCH", "0");
  if (date >= 0) define(source, "PS_DATE", std::to_string(date));
  if (alpha_only) {
    define(source, "PS_ONLY_ALPHA", "1");
    define(source, "PS_NO_ABLEND", "0");
    for (const char *part : {"PS_BLEND_A", "PS_BLEND_B", "PS_BLEND_C", "PS_BLEND_D"})
      define(source, part, "0");
  }
  auto result = make_kernel(lower_gs_shuffle(std::move(source)), DkStage_Fragment);
  device.pixels.emplace(key, result);
  return result;
}

KernelPtr hardware_vertex(uint8_t selector, float point_size) {
  const bool fst = selector & 1, iip = selector & 4, point = selector & 8;
  const uint64_t key = (uint64_t(std::bit_cast<uint32_t>(point_size)) << 32) | (selector & 13);
  if (auto found = device.vertices.find(key); found != device.vertices.end()) return found->second;
  const std::string header = "#version 450 core\n#define pGL_ES 1\n"
      "#define VERTEX_SHADER 1\n#define HAS_CLIP_CONTROL 1\n#define vs_main main\n";
  const std::string definitions = "#define VS_INT_FST " + std::to_string(!fst) +
      "\n#define VS_IIP " + std::to_string(iip) + "\n#define VS_POINT_SIZE " +
      std::to_string(point) + "\n#define VS_POINT_SIZE_VALUE " + std::to_string(point_size) + "\n";
  auto result = make_kernel(header + definitions + device.common + device.tfx_vertex, DkStage_Vertex);
  device.vertices.emplace(key, result);
  return result;
}

KernelPtr hardware_geometry(uint8_t selector) {
  if (!(selector & 4)) return {};
  const unsigned topology = selector & 3;
  const unsigned key = topology | ((selector & 8) ? 8 : 0);
  if (device.geometry[key]) return device.geometry[key];
  const std::string header = "#version 450 core\n#define pGL_ES 1\n#define GEOMETRY_SHADER 1\n"
      "#define HAS_CLIP_CONTROL 1\n#define gs_main main\n";
  const std::string definitions = "#define GS_POINT " + std::to_string(topology == 0) +
      "\n#define GS_LINE " + std::to_string(topology == 1) + "\n#define GS_IIP " +
      std::to_string(bool(selector & 8)) + "\n";
  device.geometry[key] = make_kernel(header + definitions + device.common + device.tfx_vertex,
                                     DkStage_Geometry);
  return device.geometry[key];
}

DkSampler hardware_sampler(uint8_t selector) {
  DkSampler sampler = point_sampler(selector & 4);
  const unsigned mip = (selector >> 3) & 7;
  sampler.minFilter = mip >= 2 ? (mip >= 4 ? DkFilter_Linear : DkFilter_Nearest) : sampler.magFilter;
  sampler.mipFilter = mip < 2 ? DkMipFilter_None : (mip == 3 || mip == 5) ?
      DkMipFilter_Linear : DkMipFilter_Nearest;
  sampler.lodClampMin = -1000.0f;
  sampler.lodClampMax = selector & 128 ? 0.25f : 1000.0f;
  sampler.wrapMode[0] = selector & 1 ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
  sampler.wrapMode[1] = selector & 2 ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
  if (selector & 64) sampler.maxAnisotropy = device.anisotropy;
  return sampler;
}

void hardware_constants(const DrawConfig &config, const KernelPtr &geometry) {
  if (device.uniform_generation != gpu().generation()) {
    device.uniform_generation = gpu().generation();
    device.vs_valid = device.ps_valid = false;
  }
  auto upload = [](const void *data, unsigned size) {
    const uint32_t rounded = align_up(size, DK_UNIFORM_BUF_ALIGNMENT);
    const Upload memory = gpu().upload(rounded);
    if (!memory.cpu) fatal_error("Deko3D: out of constant buffer memory");
    memset(memory.cpu, 0, rounded);
    memcpy(memory.cpu, data, size);
    return memory.gpu;
  };
  if (!device.vs_valid || memcmp(device.vs_constants.data(), config.cb_vs, sizeof(config.cb_vs))) {
    memcpy(device.vs_constants.data(), config.cb_vs, sizeof(config.cb_vs));
    device.vs_uniform = upload(config.cb_vs, sizeof(config.cb_vs));
    device.vs_valid = true;
  }
  if (!device.ps_valid || memcmp(device.ps_constants.data(), config.cb_ps, sizeof(config.cb_ps))) {
    memcpy(device.ps_constants.data(), config.cb_ps, sizeof(config.cb_ps));
    device.ps_uniform = upload(config.cb_ps, sizeof(config.cb_ps));
    device.ps_valid = true;
  }
  bind_uniform(DkStage_Vertex, 1, device.vs_uniform, 256);
  bind_uniform(DkStage_Vertex, 0, device.ps_uniform, 256);
  bind_uniform(DkStage_Fragment, 0, device.ps_uniform, 256);
  if (geometry) bind_uniform(DkStage_Geometry, 1, device.vs_uniform, 256);
}

void send_draw(const DrawConfig &config) {
  static constexpr DkPrimitive primitives[]{DkPrimitive_Points, DkPrimitive_Lines, DkPrimitive_Triangles};
  const DkPrimitive primitive = primitives[config.topology];
  auto draw = [&](uint32_t first, uint32_t count, bool barrier) {
    if (barrier) gpu().barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
    dkCmdBufDrawIndexed(gpu().commands(), primitive, count, 1, first, 0, 0);
  };
  if (config.drawlist) {
    uint32_t first = 0;
    for (const size_t *group = config.drawlist->begin; group != config.drawlist->end; ++group) {
      const uint64_t count = uint64_t(*group) * config.indices_per_primitive;
      if (!count || count > config.index_count - first) fatal_error("Deko3D: invalid draw split");
      draw(first, count, true);
      first += count;
    }
    if (first != config.index_count) fatal_error("Deko3D: incomplete draw split");
  } else if (config.full_barrier) {
    for (unsigned first = 0; first < config.index_count; first += config.indices_per_primitive)
      draw(first, std::min(config.indices_per_primitive, config.index_count - first), true);
  } else draw(0, config.index_count, gs_draw_starts_with_barrier(config));
}

void bind_draw(const DrawConfig &config, Image *color, Image *depth, NativeTexture *primid,
                const KernelPtr &vertex, const KernelPtr &pixel, const KernelPtr &geometry,
                DkGpuAddr vertices, DkGpuAddr indices, bool alpha = false, bool primitive_init = false) {
  gpu().reserve_descriptors(4);
  bind_target(color, depth, &config.scissor);
  bind_native_shaders(vertex, pixel, geometry);
  vertex_format(true);
  dkCmdBufBindVtxBuffer(gpu().commands(), 0, vertices, config.vertex_count * 32);
  dkCmdBufBindIdxBuffer(gpu().commands(), DkIdxFormat_Uint32, indices);
  hardware_constants(config, geometry);
  bind_descriptors();
  bind_native_texture(texture(config.texture), hardware_sampler(config.sampler), 0);
  bind_native_texture(texture(config.palette), point_sampler(), 1);
  bind_native_texture(pixel->source.reads_framebuffer ? texture(config.rt) : nullptr, point_sampler(), 2);
  bind_native_texture(primid, point_sampler(), 3);

  if (!primitive_init && !gs_draw_starts_with_barrier(config)) {
    gpu().prepare_sampled_image(config.texture ? texture(config.texture)->image.get() : nullptr);
    gpu().prepare_sampled_image(config.palette ? texture(config.palette)->image.get() : nullptr);
    if (pixel->source.reads_framebuffer && config.rt)
      gpu().prepare_sampled_image(texture(config.rt)->image.get());
    if (primid) gpu().prepare_sampled_image(primid->image.get());
  }
  auto dss = depth_state(config.depth, depth != nullptr);
  if (primitive_init) dss.depthWriteEnable = false;
  if (alpha) {
    if (dss.depthWriteEnable) { dss.depthWriteEnable = false; dss.depthCompareOp = DkCompareOp_Gequal; }
  }
  static constexpr DkBlendFactor factors[]{
      DkBlendFactor_SrcColor, DkBlendFactor_InvSrcColor, DkBlendFactor_DstColor, DkBlendFactor_InvDstColor,
      DkBlendFactor_Src1Color, DkBlendFactor_InvSrc1Color, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
      DkBlendFactor_DstAlpha, DkBlendFactor_InvDstAlpha, DkBlendFactor_Src1Alpha, DkBlendFactor_InvSrc1Alpha,
      DkBlendFactor_ConstColor, DkBlendFactor_InvConstColor, DkBlendFactor_One, DkBlendFactor_Zero};
  const unsigned operation = (config.blend >> 2) & 63;
  const unsigned source = (config.blend >> 8) & 255, destination = (config.blend >> 16) & 255;
  if (operation > 2 || source >= 16 || destination >= 16) fatal_error("Deko3D: invalid blend state");
  static constexpr DkBlendOp operations[]{DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_RevSub};
  native_state(primitive_init ? 1 : alpha ? 8 : config.color_mask, dss,
      primitive_init || (!alpha && (config.blend & 1)),
      primitive_init ? DkBlendFactor_One : factors[source],
      primitive_init ? DkBlendFactor_One : factors[destination],
      primitive_init ? DkBlendOp_Min : operations[operation]);
  const float constant = float(config.blend >> 24) / 128.0f;
  dkCmdBufSetBlendConst(gpu().commands(), constant, constant, constant, constant);
  dkCmdBufSetStencil(gpu().commands(), DkFace_FrontAndBack, 1, 1, 1);
  dkCmdBufSetPointSize(gpu().commands(), 1.0f);
  const float upscale = *reinterpret_cast<const float *>(core_address(profile->upscale_address));
  dkCmdBufSetLineWidth(gpu().commands(), gs_line_width(config.flags, upscale));
}

void mark_draw_targets(const DrawConfig &config, Image *color, Image *depth,
                         bool alpha = false, bool primitive_init = false) {
  if (primitive_init || alpha || config.color_mask) gpu().image_written(color);
  if ((!primitive_init && !alpha && (config.depth & 4)) ||
      ((config.depth & 24) == 24)) gpu().image_written(depth);
}
}

void render_hardware(void *, DrawConfig *input) {
  if (!input || !input->vertex_count || !input->index_count) return;
  DrawConfig config = *input;
  if (config.topology > 2 || !config.indices_per_primitive || !config.vertices || !config.indices ||
      config.vertex_count > 1024 * 1024 || config.index_count > 4 * 1024 * 1024)
    fatal_error("Deko3D: invalid GS hardware draw layout");
  auto *rt = texture(config.rt), *ds = texture(config.ds);
  if (!rt && !ds) return;
  auto pixel = pixel_kernel(config.ps);
  const float upscale = *reinterpret_cast<const float *>(core_address(profile->upscale_address));

  const auto vertex = hardware_vertex(config.vs | (config.topology == 0 ? 8 : 0),
                                       config.vs & 8 ? upscale : 1.0f);
  const auto geometry = hardware_geometry(config.gs);
  Image *size = rt ? rt->image.get() : ds->image.get();
  NativeTexture *primid = nullptr, *hdr = nullptr;
  const FloatRect area{float(config.drawarea.x), float(config.drawarea.y),
                       float(config.drawarea.z), float(config.drawarea.w)};
  const FloatRect source_area{area.x / size->width, area.y / size->height,
                              area.z / size->width, area.w / size->height};
  if (config.destination_alpha == 1 || config.destination_alpha == 2) {
    if (!rt || !ds) fatal_error("Deko3D: DATE requires a color and stencil target");
    if (config.destination_alpha == 2) clear_stencil(ds, 1);
    else {
      clear_stencil(ds, 0);
      draw_rect(rt, source_area, nullptr, area, convert_kernel((config.flags & 1) ? 2 : 3),
                false, 0, false, ds->image.get(), true);
    }
  } else if (config.destination_alpha == 3) {
    if (!rt) fatal_error("Deko3D: primitive DATE has no render target");
    primid = temporary_texture(size->width, size->height, 7);
    const auto init = utility_kernel("shaders/opengl/convert.glsl",
        (config.flags & 1) ? "ps_stencil_image_init_1" : "ps_stencil_image_init_0", DkStage_Fragment);
    draw_rect(rt, source_area, primid->image.get(), area, init, false);
  }
  if (pixel->hdr && rt) {
    hdr = temporary_texture(size->width, size->height, 2);
    draw_rect(rt, source_area, hdr->image.get(), area, convert_kernel(4), false);
  }

  gpu().reserve_descriptors(32);
  const uint32_t vertex_bytes = config.vertex_count * 32, index_bytes = config.index_count * 4;
  const Upload vertices = gpu().upload(vertex_bytes, 16), indices = gpu().upload(index_bytes, 4);
  if (!vertices.cpu || !indices.cpu) fatal_error("Deko3D: out of GS upload memory");
  memcpy(vertices.cpu, config.vertices, vertex_bytes);
  memcpy(indices.cpu, config.indices, index_bytes);
  Image *color = hdr ? hdr->image.get() : rt ? rt->image.get() : nullptr;
  Image *depth = ds ? ds->image.get() : nullptr;
  if (primid) {
    bind_draw(config, primid->image.get(), depth, nullptr, vertex, pixel, geometry,
               vertices.gpu, indices.gpu, false, true);
    static constexpr DkPrimitive primitives[]{DkPrimitive_Points, DkPrimitive_Lines, DkPrimitive_Triangles};
    gpu().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
    dkCmdBufDrawIndexed(gpu().commands(), primitives[config.topology], config.index_count, 1, 0, 0, 0);
    mark_draw_targets(config, primid->image.get(), depth, false, true);
    pixel = pixel_kernel(config.ps, 3);
  }
  auto pass = [&](KernelPtr shader, bool alpha) {
    bind_draw(config, color, depth, primid, vertex, shader, geometry, vertices.gpu, indices.gpu, alpha);
    send_draw(config);
    mark_draw_targets(config, color, depth, alpha);
  };
  pass(pixel, false);
  if (config.flags & 4) pass(pixel_kernel(config.ps, primid ? 3 : -1, true), true);
  if (config.second_pass.enable) {
    config.color_mask = config.second_pass.color_mask;
    config.depth = config.second_pass.depth;
    memcpy(config.cb_ps + 12, &config.second_pass.aref, sizeof(float));
    pass(pixel_kernel(config.second_pass.ps, primid ? 3 : -1), false);
    if (config.flags & 8) pass(pixel_kernel(config.second_pass.ps, primid ? 3 : -1, true), true);
  }
  if (hdr) draw_rect(hdr, source_area, rt->image.get(), area, convert_kernel(5), false);
  if (rt) rt->abi.state = 0;
  if (ds) ds->abi.state = 0;
}

void stretch_rect(void *, TextureAbi *source, const FloatRect *sr, TextureAbi *target,
                  const FloatRect *dr, unsigned shader, bool linear) {
  if (!source || !target || !sr || !dr) return;
  auto *destination = texture(target);
  draw_rect(texture(source), *sr, destination->image.get(), *dr, convert_kernel(shader), linear);
  target->state = 0;
}

void multi_stretch(void *core, const MultiRect *rects, unsigned count, TextureAbi *destination,
                   unsigned shader) {
  if (!rects || !destination) return;
  if (count > 1024 * 1024) fatal_error("Deko3D: invalid rectangle list");
  const auto pixel = convert_kernel(shader);
  const auto vertex = utility_kernel("shaders/opengl/convert.glsl", "vs_main", DkStage_Vertex);
  Image *target = texture(destination)->image.get();
  const auto attachments = utility_targets(target);
  const auto dss = attachments.depth_stencil();
  struct Vertex { float position[4], uv[2]; uint32_t pad, color; };

  for (unsigned first = 0; first < count;) {
    const unsigned batch = std::min(count - first, 1024u);
    gpu().reserve_descriptors(batch);
    const Upload vertices = gpu().upload(batch * 4 * sizeof(Vertex), 16);
    const Upload indices = gpu().upload(batch * 6 * sizeof(uint16_t), 4);
    if (!vertices.cpu || !indices.cpu) fatal_error("Deko3D: out of rectangle batch memory");
    auto *vbo = static_cast<Vertex *>(vertices.cpu);
    auto *ibo = static_cast<uint16_t *>(indices.cpu);
    for (unsigned i = 0; i < batch; ++i) {
      const auto &rect = rects[first + i];
      const auto &sr = rect.source_rect;
      const auto &dr = rect.target_rect;
      const float x0 = dr.x * 2 / target->width - 1, x1 = dr.z * 2 / target->width - 1;
      const float y0 = dr.y * 2 / target->height - 1, y1 = dr.w * 2 / target->height - 1;
      vbo[i*4+0] = {{x0,y0,0,0},{sr.x,sr.y},0,0xffffffff};
      vbo[i*4+1] = {{x1,y0,0,0},{sr.z,sr.y},0,0xffffffff};
      vbo[i*4+2] = {{x0,y1,0,0},{sr.x,sr.w},0,0xffffffff};
      vbo[i*4+3] = {{x1,y1,0,0},{sr.z,sr.w},0,0xffffffff};
      const uint16_t base = i * 4;
      const uint16_t triangle[]{base, uint16_t(base+1), uint16_t(base+2),
                               uint16_t(base+2), uint16_t(base+1), uint16_t(base+3)};
      memcpy(ibo + i * 6, triangle, sizeof(triangle));
    }
    bind_target(attachments.color, attachments.depth);
    bind_native_shaders(vertex, pixel);
    vertex_format(false);
    dkCmdBufBindVtxBuffer(gpu().commands(), 0, vertices.gpu, vertices.size);
    dkCmdBufBindIdxBuffer(gpu().commands(), DkIdxFormat_Uint16, indices.gpu);
    bind_descriptors();
    bind_kernel_uniforms(*pixel, DkStage_Fragment);
    for (unsigned i = 0; i < batch;) {
      const auto &rect = rects[first + i];
      unsigned end = i + 1;
      while (end < batch && rects[first+end].source == rect.source &&
             rects[first+end].linear == rect.linear && rects[first+end].color_mask == rect.color_mask) ++end;
      if (rect.source) {
        native_state(attachments.color_mask(rect.color_mask), dss, false);
        bind_native_texture(texture(rect.source), point_sampler(rect.linear), 0);
        gpu().prepare_sampled_image(texture(rect.source)->image.get());
        dkCmdBufDrawIndexed(gpu().commands(), DkPrimitive_Triangles, (end-i)*6, 1, i*6, 0, 0);
        if (attachments.color_mask(rect.color_mask)) gpu().image_written(attachments.color);
        if (dss.depthWriteEnable) gpu().image_written(attachments.depth);
      }
      i = end;
    }
    first += batch;
  }
  destination->state = 0;
  (void)core;
}

void present_rect(void *, TextureAbi *source, const FloatRect *sr, TextureAbi *destination,
                  const FloatRect *dr, unsigned shader, float time, bool linear) {
  if (!source || !sr || !dr) return;
  static constexpr const char *names[]{"ps_copy", "ps_filter_scanlines", "ps_filter_diagonal",
      "ps_filter_triangular", "ps_filter_complex", "ps_filter_lottes"};
  if (shader >= std::size(names)) fatal_error("Deko3D: invalid presentation shader");
  Image *target = destination ? texture(destination)->image.get() : gpu().backbuffer();
  if (!target) return;
  if (!destination && source->width > 0 && source->height > 0)
    saltynx_set_resolution(source->width, source->height);
  auto pixel = utility_kernel("shaders/opengl/present.glsl", names[shader], DkStage_Fragment);
  const float src_size[]{(sr->z - sr->x) * source->width, (sr->w - sr->y) * source->height};
  const float dst_size[]{dr->z - dr->x, dr->w - dr->y};
  const float resolution[]{float(target->width), float(target->height)};
  const float reciprocal[]{1.0f / target->width, 1.0f / target->height};
  const float source_resolution[]{float(source->width), float(source->height)};
  const float source_reciprocal[]{1.0f / source->width, 1.0f / source->height};
  pixel->uniform("u_source_rect", sr, sizeof(*sr));
  pixel->uniform("u_target_rect", dr, sizeof(*dr));
  pixel->uniform("u_source_size", src_size, sizeof(src_size));
  pixel->uniform("u_target_size", dst_size, sizeof(dst_size));
  pixel->uniform("u_target_resolution", resolution, sizeof(resolution));
  pixel->uniform("u_rcp_target_resolution", reciprocal, sizeof(reciprocal));
  pixel->uniform("u_source_resolution", source_resolution, sizeof(source_resolution));
  pixel->uniform("u_rcp_source_resolution", source_reciprocal, sizeof(source_reciprocal));
  pixel->uniform("u_time", &time, sizeof(time));

  const FloatRect flipped{sr->x, sr->w, sr->z, sr->y};
  draw_rect(texture(source), flipped, target, *dr, pixel, linear);
}

void update_clut(void *, TextureAbi *source, unsigned x, unsigned y, TextureAbi *destination,
                 unsigned offset, unsigned count) {
  if (!source || !destination) return;
  auto pixel = utility_kernel("shaders/opengl/convert.glsl", count == 16 ? "ps_convert_clut_4" :
                               "ps_convert_clut_8", DkStage_Fragment);
  const unsigned values[]{x, y, offset};
  pixel->uniform("offset", values, sizeof(values));
  pixel->uniform("scale", source->scale, sizeof(source->scale));
  draw_rect(texture(source), {0,0,1,1}, texture(destination)->image.get(),
             gs_clut_rect(count), pixel, false);
  destination->state = 0;
}

void merge_outputs(void *, TextureAbi **sources, const FloatRect *sr, TextureAbi *destination,
                    const FloatRect *dr, const uint64_t *pmode, const uint64_t *extbuf,
                    const FloatRect *background, bool linear) {
  if (!sources || !sr || !destination || !dr || !pmode || !extbuf || !background) return;
  const bool en1 = *pmode & 1, en2 = *pmode & 2, constant_alpha = *pmode & 32;
  const bool preserve_alpha = *pmode & 64, background_selected = *pmode & 128;
  const unsigned input = (*extbuf >> 20) & 3;
  const bool feedback2 = en2 && sources[2] && input == 1;
  const bool feedback1 = en1 && sources[2] && input == 0;
  const FloatRect full{0,0,1,1};
  auto *target = texture(destination);
  clear_color(target, &background->x);
  if (sources[1] && (!background_selected || feedback2))
    draw_rect(texture(sources[1]), sr[1], target->image.get(), background_selected ? dr[2] : dr[1],
               convert_kernel(0), linear);
  KernelPtr yuv;
  if (feedback1 || feedback2) {
    yuv = utility_kernel("shaders/opengl/convert.glsl", "ps_yuv", DkStage_Fragment);
    const int mode[]{int((*extbuf >> 23) & 3), int((*extbuf >> 25) & 3)};
    yuv->uniform("EMOD", mode, sizeof(mode));
  }
  if (feedback2) draw_rect(target, full, texture(sources[2])->image.get(), dr[2], yuv, linear);
  if (feedback2 && background_selected) clear_color(target, &background->x);
  if (sources[0]) {
    auto pixel = utility_kernel("shaders/opengl/merge.glsl", constant_alpha ? "ps_main1" : "ps_main0",
                                 DkStage_Fragment);
    pixel->uniform("BGColor", background, sizeof(*background));
    draw_rect(texture(sources[0]), sr[0], target->image.get(), dr[0], pixel, linear,
               preserve_alpha ? 7 : 15, true);
  }
  if (feedback1) draw_rect(target, full, texture(sources[2])->image.get(), dr[2], yuv, linear);
  destination->state = 0;
}

void interlace(void *, TextureAbi *source, TextureAbi *destination, int shader, bool linear,
                float yoffset, int buffer_index) {
  if (!source || !destination) return;
  if (shader < 0 || shader > 4) fatal_error("Deko3D: invalid interlace shader");
  auto pixel = utility_kernel("shaders/opengl/interlace.glsl", "ps_main" + std::to_string(shader),
                               DkStage_Fragment);
  const float parameters[]{float(buffer_index), 1.0f / destination->height,
                            float(destination->height), 0.08f};
  pixel->uniform("ZrH", parameters, sizeof(parameters));
  draw_rect(texture(source), {0,0,1,1}, texture(destination)->image.get(),
             {0,yoffset,float(destination->width),float(destination->height) + yoffset}, pixel, linear);
  destination->state = 0;
}

void fxaa(void *, TextureAbi *source, TextureAbi *destination) {
  if (!source || !destination) return;
  auto pixel = utility_kernel("shaders/common/fxaa.fx", "main", DkStage_Fragment,
                               "#define FXAA_GLSL_130 1\n");
  draw_rect(texture(source), {0,0,1,1}, texture(destination)->image.get(),
             {0,0,float(destination->width),float(destination->height)}, pixel, true);
  destination->state = 0;
}

void shadeboost(void *, TextureAbi *source, TextureAbi *destination, const float *parameters) {
  if (!source || !destination || !parameters) return;
  auto pixel = utility_kernel("shaders/opengl/shadeboost.glsl", "ps_main", DkStage_Fragment);
  pixel->uniform("params", parameters, sizeof(float) * 4);
  draw_rect(texture(source), {0,0,1,1}, texture(destination)->image.get(),
             {0,0,float(destination->width),float(destination->height)}, pixel, false);
  destination->state = 0;
}

bool sharpen(void *, TextureAbi *source, TextureAbi *destination, bool sharpen_only,
               const uint32_t *constants) {
  if (!source || !destination || !constants) return false;
  auto compute = utility_kernel("shaders/opengl/cas.glsl", "main", DkStage_Compute,
      std::string("#define CAS_SHARPEN_ONLY ") + (sharpen_only ? "true\n" : "false\n"));
  compute->uniform("const0", constants, 16);
  compute->uniform("const1", constants + 4, 16);
  compute->uniform("srcOffset", constants + 8, 8);
  gpu().reserve_descriptors(2);
  bind_descriptors();
  bind_native_texture(texture(source), point_sampler(), 0, DkStage_Compute);
  const DkImageView view = texture(destination)->image->view();
  const DkSampler sampler = point_sampler();
  const DkResHandle storage = gpu().descriptor(view, sampler, true);
  dkCmdBufBindImage(gpu().commands(), DkStage_Compute, 0, storage);
  const DkShader *shader = &compute->binary->shader;
  dkCmdBufBindShaders(gpu().commands(), DkStageFlag_Compute, &shader, 1);
  bind_kernel_uniforms(*compute, DkStage_Compute);
  gpu().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
  dkCmdBufDispatchCompute(gpu().commands(), (destination->width + 15) / 16,
                          (destination->height + 15) / 16, 1);
  gpu().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
  destination->state = 0;
  invalidate_native_state();
  return true;
}
}
