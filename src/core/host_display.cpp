#include "host_display.h"
#include "common/log.h"
#include "common/string_util.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"
#include <cmath>
#include <cstring>
#include <vector>
Log_SetChannel(HostDisplay);

HostDisplayTexture::~HostDisplayTexture() = default;

HostDisplay::~HostDisplay() = default;

void HostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  m_window_width = new_window_width;
  m_window_height = new_window_height;
}

std::tuple<s32, s32, s32, s32> HostDisplay::CalculateDrawRect() const
{
  const s32 window_width = m_window_width;
  const s32 window_height = m_window_height - m_display_top_margin;
  const float window_ratio = static_cast<float>(window_width) / static_cast<float>(window_height);

  float scale;
  int left, top, width, height;
  if (window_ratio >= m_display_aspect_ratio)
  {
    width = static_cast<int>(static_cast<float>(window_height) * m_display_aspect_ratio);
    height = static_cast<int>(window_height);
    scale = static_cast<float>(window_height) / static_cast<float>(m_display_height);
    left = (window_width - width) / 2;
    top = 0;
  }
  else
  {
    width = static_cast<int>(window_width);
    height = static_cast<int>(float(window_width) / m_display_aspect_ratio);
    scale = static_cast<float>(window_width) / static_cast<float>(m_display_width);
    left = 0;
    top = (window_height - height) / 2;
  }

  // add in padding
  left += static_cast<s32>(static_cast<float>(m_display_area.left) * scale);
  top += static_cast<s32>(static_cast<float>(m_display_area.top) * scale);
  width -= static_cast<s32>(static_cast<float>(m_display_area.left + (m_display_width - m_display_area.right)) * scale);
  height -=
    static_cast<s32>(static_cast<float>(m_display_area.top + (m_display_height - m_display_area.bottom)) * scale);

  // add in margin
  top += m_display_top_margin;
  return std::tie(left, top, width, height);
}

bool HostDisplay::WriteTextureToFile(const void* texture_handle, u32 x, u32 y, u32 width, u32 height,
                                     const char* filename, bool clear_alpha /* = true */, bool flip_y /* = false */,
                                     u32 resize_width /* = 0 */, u32 resize_height /* = 0 */)
{
  std::vector<u32> texture_data(width * height);
  u32 texture_data_stride = sizeof(u32) * width;
  if (!DownloadTexture(texture_handle, x, y, width, height, texture_data.data(), texture_data_stride))
  {
    Log_ErrorPrintf("Texture download failed");
    return false;
  }

  const char* extension = std::strrchr(filename, '.');
  if (!extension)
  {
    Log_ErrorPrintf("Unable to determine file extension for '%s'", filename);
    return false;
  }

  if (clear_alpha)
  {
    for (u32& pixel : texture_data)
      pixel |= 0xFF000000;
  }

  if (flip_y)
  {
    std::vector<u32> temp(width);
    for (u32 flip_row = 0; flip_row < (height / 2); flip_row++)
    {
      u32* top_ptr = &texture_data[flip_row * width];
      u32* bottom_ptr = &texture_data[((height - 1) - flip_row) * width];
      std::memcpy(temp.data(), top_ptr, texture_data_stride);
      std::memcpy(top_ptr, bottom_ptr, texture_data_stride);
      std::memcpy(bottom_ptr, temp.data(), texture_data_stride);
    }
  }

  if (resize_width > 0 && resize_height > 0 && (resize_width != width || resize_height != height))
  {
    std::vector<u32> resized_texture_data(resize_width * resize_height);
    u32 resized_texture_stride = sizeof(u32) * resize_width;
    if (!stbir_resize_uint8(reinterpret_cast<u8*>(texture_data.data()), width, height, texture_data_stride,
                            reinterpret_cast<u8*>(resized_texture_data.data()), resize_width, resize_height,
                            resized_texture_stride, 4))
    {
      Log_ErrorPrintf("Failed to resize texture data from %ux%u to %ux%u", width, height, resize_width, resize_height);
      return false;
    }

    width = resize_width;
    height = resize_height;
    texture_data = std::move(resized_texture_data);
    texture_data_stride = resized_texture_stride;
  }

  bool result;
  if (StringUtil::Strcasecmp(extension, ".png") == 0)
  {
    result = (stbi_write_png(filename, width, height, 4, texture_data.data(), texture_data_stride) != 0);
  }
  else if (StringUtil::Strcasecmp(filename, ".jpg") == 0)
  {
    result = (stbi_write_jpg(filename, width, height, 4, texture_data.data(), 95) != 0);
  }
  else if (StringUtil::Strcasecmp(filename, ".tga") == 0)
  {
    result = (stbi_write_tga(filename, width, height, 4, texture_data.data()) != 0);
  }
  else if (StringUtil::Strcasecmp(filename, ".bmp") == 0)
  {
    result = (stbi_write_bmp(filename, width, height, 4, texture_data.data()) != 0);
  }
  else
  {
    Log_ErrorPrintf("Unknown extension in filename '%s': '%s'", filename, extension);
    return false;
  }

  if (!result)
  {
    Log_ErrorPrintf("Failed to save texture to '%s'", filename);
    return false;
  }

  return true;
}

bool HostDisplay::WriteDisplayTextureToFile(const char* filename, bool full_resolution /* = true */,
                                            bool apply_aspect_ratio /* = true */)
{
  if (!m_display_texture_handle)
    return false;

  s32 resize_width = 0;
  s32 resize_height = 0;
  if (apply_aspect_ratio && full_resolution)
  {
    if (m_display_aspect_ratio > 1.0f)
    {
      resize_width = m_display_texture_view_width;
      resize_height = static_cast<s32>(static_cast<float>(resize_width) / m_display_aspect_ratio);
    }
    else
    {
      resize_height = m_display_texture_view_height;
      resize_width = static_cast<s32>(static_cast<float>(resize_height) * m_display_aspect_ratio);
    }
  }
  else if (apply_aspect_ratio)
  {
    const auto [left, top, right, bottom] = CalculateDrawRect();
    resize_width = right - left;
    resize_height = bottom - top;
  }
  else if (!full_resolution)
  {
    const auto [left, top, right, bottom] = CalculateDrawRect();
    const float ratio =
      static_cast<float>(m_display_texture_view_width) / static_cast<float>(m_display_texture_view_height);
    if (ratio > 1.0f)
    {
      resize_width = right - left;
      resize_height = static_cast<s32>(static_cast<float>(resize_width) / ratio);
    }
    else
    {
      resize_height = bottom - top;
      resize_width = static_cast<s32>(static_cast<float>(resize_height) * ratio);
    }
  }

  if (resize_width < 0)
    resize_width = 1;
  if (resize_height < 0)
    resize_height = 1;

  const bool flip_y = (m_display_texture_view_height < 0);
  s32 read_height = m_display_texture_view_height;
  s32 read_y = m_display_texture_view_y;
  if (flip_y)
  {
    read_height = -m_display_texture_view_height;
    read_y = (m_display_texture_height - read_height) - (m_display_texture_height - m_display_texture_view_y);
  }

  return WriteTextureToFile(m_display_texture_handle, m_display_texture_view_x, read_y, m_display_texture_view_width,
                            read_height, filename, true, flip_y, static_cast<u32>(resize_width),
                            static_cast<u32>(resize_height));
}
