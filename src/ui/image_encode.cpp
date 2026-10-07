/**
 * @file        ui/image_encode.cpp
 * @brief       PNG writer for captured frames. See image_encode.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/image_encode.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <system_error>
#include <vector>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace rex::ui {

#if REX_PLATFORM_WIN32

namespace {

// The WIC GUIDs (values from wincodec.h), defined here so nothing has to
// instantiate wincodec.h's DEFINE_GUIDs or link windowscodecs.lib for them.
constexpr GUID kClsidWicImagingFactory = {
    0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
constexpr GUID kContainerFormatPng = {
    0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
constexpr GUID kPixelFormat24bppBGR = {
    0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};

std::string HresultText(const char* what, HRESULT hr) {
  char text[96];
  std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
  return text;
}

bool Encode(const RawImage& image, const std::filesystem::path& path, std::string& error) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    error = HresultText("CoCreateInstance(WICImagingFactory)", hr);
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(hr = factory->CreateStream(&stream)) ||
      FAILED(hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
    error = HresultText("opening the file", hr);
    return false;
  }
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(hr = factory->CreateEncoder(kContainerFormatPng, nullptr, &encoder)) ||
      FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    error = HresultText("creating the PNG encoder", hr);
    return false;
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (FAILED(hr = encoder->CreateNewFrame(&frame, &properties)) ||
      FAILED(hr = frame->Initialize(properties.Get())) ||
      FAILED(hr = frame->SetSize(image.width, image.height))) {
    error = HresultText("creating the PNG frame", hr);
    return false;
  }
  WICPixelFormatGUID format = kPixelFormat24bppBGR;
  if (FAILED(hr = frame->SetPixelFormat(&format)) || format != kPixelFormat24bppBGR) {
    error = HresultText("setting the pixel format", hr);
    return false;
  }
  // R8 G8 B8 X8 -> B8 G8 R8, a band of rows at a time.
  const uint32_t row_bytes = image.width * 3;
  constexpr uint32_t kBandRows = 128;
  std::vector<uint8_t> band(size_t(row_bytes) * kBandRows);
  for (uint32_t y = 0; y < image.height; y += kBandRows) {
    const uint32_t rows = std::min(kBandRows, image.height - y);
    for (uint32_t row = 0; row < rows; ++row) {
      const uint8_t* source = image.data.data() + image.stride * (y + row);
      uint8_t* dest = band.data() + size_t(row_bytes) * row;
      for (uint32_t x = 0; x < image.width; ++x) {
        dest[x * 3 + 0] = source[x * 4 + 2];
        dest[x * 3 + 1] = source[x * 4 + 1];
        dest[x * 3 + 2] = source[x * 4 + 0];
      }
    }
    if (FAILED(hr = frame->WritePixels(rows, row_bytes, row_bytes * rows, band.data()))) {
      error = HresultText("writing the pixels", hr);
      return false;
    }
  }
  if (FAILED(hr = frame->Commit()) || FAILED(hr = encoder->Commit())) {
    error = HresultText("finishing the PNG", hr);
    return false;
  }
  return true;
}

}  // namespace

bool WriteImagePng(const RawImage& image, const std::filesystem::path& path, std::string* error) {
  std::string message;
  if (!image.width || !image.height || image.stride < size_t(image.width) * 4 ||
      image.data.size() < image.stride * (image.height - 1) + size_t(image.width) * 4) {
    message = "empty or malformed image";
  } else if (path.empty()) {
    message = "no file name";
  }
  if (message.empty()) {
    std::error_code ec;
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::filesystem::path partial = path;
    partial += L".partial";
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool encoded = Encode(image, partial, message);
    if (SUCCEEDED(co)) {
      CoUninitialize();
    }
    if (encoded) {
      std::filesystem::rename(partial, path, ec);
      if (ec) {
        message = "renaming the finished file: " + ec.message();
      }
    }
    if (!message.empty()) {
      std::filesystem::remove(partial, ec);
    }
  }
  if (error) {
    *error = message;
  }
  return message.empty();
}

#else  // REX_PLATFORM_WIN32

bool WriteImagePng(const RawImage& image, const std::filesystem::path& path, std::string* error) {
  (void)image;
  (void)path;
  if (error) {
    *error = "PNG writing is not implemented on this platform";
  }
  return false;
}

#endif  // REX_PLATFORM_WIN32

}  // namespace rex::ui
