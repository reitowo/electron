// Copyright (c) 2023 Salesforce, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/renderer/api/electron_api_web_utils.h"

#include "base/strings/string_number_conversions_internal.h"
#include "components/viz/common/resources/shared_image_format_utils.h"
#include "media/base/format_utils.h"
#include "media/base/video_frame.h"
#include "shell/common/gin_converters/blink_converter.h"
#include "shell/common/gin_converters/gfx_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/error_thrower.h"
#include "shell/common/node_includes.h"
#include "third_party/blink/public/web/web_blob.h"
#include "third_party/blink/renderer/bindings/core/v8/to_v8_traits.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_blob.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_device.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_texture.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/modules/webcodecs/video_frame.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_texture.h"
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"

namespace {

struct ExternalSharedTexturePlane {
  // The strides and offsets in bytes to be used when accessing the buffers
  // via a memory mapping. One per plane per entry. Size in bytes of the
  // plane is necessary to map the buffers.
  uint32_t stride;
  uint64_t offset;
  uint64_t size;

  // File descriptor for the underlying memory object (usually dmabuf).
  int fd = 0;
};

struct ExternalSharedTexture {
  // The pixel format of the shared texture, RGBA or BGRA depends on platform.
  media::VideoPixelFormat pixel_format;

  // The full dimensions of the video frame data.
  gfx::Size coded_size;

  // A subsection of [0, 0, coded_size().width(), coded_size.height()].
  // In OSR case, it is expected to have the full area of the section.
  gfx::Rect visible_rect;

  // The capture timestamp, microseconds since capture start
  int64_t timestamp = 0;

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  // On Windows it is a HANDLE to the shared D3D11 texture.
  // On macOS it is a IOSurface* to the shared IOSurface.
  uintptr_t shared_texture_handle = 0;
#elif BUILDFLAG(IS_LINUX)
  std::vector<ExternalSharedTexturePlane> planes;
  uint64_t modifier = gfx::NativePixmapHandle::kNoModifier;
  bool supports_zero_copy_webgpu_import = false;
#endif
};

wgpu::TextureFormat ToWGPUFormat(const media::VideoPixelFormat& format) {
  if (format == media::VideoPixelFormat::PIXEL_FORMAT_ARGB) {
    return wgpu::TextureFormat::BGRA8Unorm;
  }
  if (format == media::VideoPixelFormat::PIXEL_FORMAT_ABGR) {
    return wgpu::TextureFormat::RGBA8Unorm;
  }
  NOTREACHED() << "Unexpected media pixel format: " << format;
}

}  // namespace

namespace gin {

template <>
struct Converter<ExternalSharedTexture> {
  static bool FromV8(v8::Isolate* isolate,
                     v8::Local<v8::Value> val,
                     ExternalSharedTexture* out) {
    if (!val->IsObject())
      return false;
    gin::Dictionary dict(isolate, val.As<v8::Object>());

    std::string pixel_format_str;
    if (dict.Get("pixelFormat", &pixel_format_str)) {
      if (pixel_format_str == "bgra")
        out->pixel_format = media::PIXEL_FORMAT_ARGB;
      else if (pixel_format_str == "rgba")
        out->pixel_format = media::PIXEL_FORMAT_ABGR;
      else
        return false;
    }

    dict.Get("codedSize", &out->coded_size);
    dict.Get("visibleRect", &out->visible_rect);
    dict.Get("timestamp", &out->timestamp);

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    v8::Local<v8::Value> handle_buf;
    if (dict.Get("sharedTextureHandle", &handle_buf) &&
        node::Buffer::HasInstance(handle_buf)) {
      char* data = node::Buffer::Data(handle_buf);
      if (node::Buffer::Length(handle_buf) ==
          sizeof(out->shared_texture_handle)) {
        memcpy(&out->shared_texture_handle, data,
               sizeof(out->shared_texture_handle));
      }
    }
#elif BUILDFLAG(IS_LINUX)
    dict.Get("supportsZeroCopyWebGpuImport",
             &out->supports_zero_copy_webgpu_import);
    std::vector<gin::Dictionary> v8_planes;
    if (dict.Get("planes", &v8_planes)) {
      out->planes.clear();
      for (auto& v8_plane : v8_planes) {
        ExternalSharedTexturePlane plane;
        v8_plane.Get("stride", &plane.stride);
        v8_plane.Get("offset", &plane.offset);
        v8_plane.Get("size", &plane.size);
        v8_plane.Get("fd", &plane.fd);
        out->planes.push_back(plane);
      }
    }
    std::string modifier_str;
    if (dict.Get("modifier", &modifier_str)) {
      base::StringToUint64(modifier_str, &out->modifier);
    }
#endif

    return true;
  }
};

}  // namespace gin

namespace electron::api::web_utils {

std::string GetPathForFile(v8::Isolate* isolate, v8::Local<v8::Value> file) {
  blink::WebBlob blob = blink::WebBlob::FromV8Value(isolate, file);
  if (blob.IsNull()) {
    gin_helper::ErrorThrower(isolate).ThrowTypeError(
        "getPathForFile expected to receive a File object but one was not "
        "provided");
    return "";
  }
  return blob.Path();
}

v8::Local<v8::Value> ImportSharedTextureToWebGPU(
    v8::Isolate* isolate,
    v8::Local<v8::Value> gpu_device,
    v8::Local<v8::Value> shared_texture_options) {
  ExternalSharedTexture shared_texture{};
  if (!gin::ConvertFromV8(isolate, shared_texture_options, &shared_texture)) {
    gin_helper::ErrorThrower(isolate).ThrowTypeError(
        "Invalid shared texture info object");
    return v8::Null(isolate);
  }

  wgpu::SharedTextureMemoryDescriptor desc;

#if BUILDFLAG(IS_WIN)
  auto handle = reinterpret_cast<HANDLE>(shared_texture.shared_texture_handle);
  wgpu::SharedTextureMemoryDXGISharedHandleDescriptor shared_handle_desc;
  shared_handle_desc.handle = handle;
  shared_handle_desc.useKeyedMutex = false;
  desc.nextInChain = &shared_handle_desc;
  desc.label = "ImportSharedTextureToWebGPU_SharedTextureMemory_DXGIHandle";
#elif BUILDFLAG(IS_APPLE)
  gmb_handle.type = gfx::IO_SURFACE_BUFFER;

  auto io_surface =
      reinterpret_cast<IOSurfaceRef>(sharedTexture.shared_texture_handle);

  wgpu::SharedTextureMemoryIOSurfaceHandleDescriptor shared_handle_desc;
  shared_handle_desc.ioSurface = io_surface;
  desc.nextInChain = &shared_handle_desc;
  desc.label = "ImportSharedTextureToWebGPU_SharedTextureMemory_IOSurface";
#elif BUILDFLAG(IS_LINUX)
  gmb_handle.type = gfx::NATIVE_PIXMAP;

  gfx::NativePixmapHandle pixmap;
  pixmap.modifier = shared_texture.modifier;
  pixmap.supports_zero_copy_webgpu_import =
      shared_texture.supports_zero_copy_webgpu_import;

  for (const auto& plane : shared_texture.planes) {
    gfx::NativePixmapPlane plane_info;
    plane_info.stride = plane.stride;
    plane_info.offset = plane.offset;
    plane_info.size = plane.size;
    plane_info.fd = base::ScopedFD(plane.fd);
    pixmap.planes.push_back(std::move(plane_info));
  }

  gmb_handle.native_pixmap_handle = std::move(pixmap);
#endif

  blink::GPUDevice* blink_device = blink::V8GPUDevice::ToWrappableUnsafe(
      isolate, gpu_device.As<v8::Object>());

  auto device = blink_device->GetHandle();
  auto shared_texture_memory = device.ImportSharedTextureMemory(&desc);

  wgpu::TextureDescriptor texture_desc;
  texture_desc.label = "ImportSharedTextureToWebGPU_TextureDescriptor";
  texture_desc.size.width = shared_texture.coded_size.width();
  texture_desc.size.height = shared_texture.coded_size.height();
  texture_desc.format = ToWGPUFormat(shared_texture.pixel_format);
  texture_desc.usage = wgpu::TextureUsage::RenderAttachment |
                       wgpu::TextureUsage::TextureBinding |
                       wgpu::TextureUsage::CopySrc;

  auto imported_texture = shared_texture_memory.CreateTexture(&texture_desc);

  blink::GPUTexture* gpu_texture =
      blink::MakeGarbageCollected<blink::GPUTexture>(
          blink_device, imported_texture,
          String::FromUTF8("ImportSharedTextureToWebGPU_Texture"));

  auto* current_script_state = blink::ScriptState::ForCurrentRealm(isolate);
  return blink::ToV8Traits<blink::GPUTexture>::ToV8(current_script_state,
                                                    gpu_texture);
}

}  // namespace electron::api::web_utils

namespace {

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  v8::Isolate* isolate = context->GetIsolate();
  gin_helper::Dictionary dict(isolate, exports);
  dict.SetMethod("getPathForFile", &electron::api::web_utils::GetPathForFile);
  dict.SetMethod("importExternalSharedTexture",
                 &electron::api::web_utils::ImportSharedTextureToWebGPU);
}

}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_renderer_web_utils, Initialize)
