#pragma once

#include <string>
#include <spa/param/video/format-utils.h>
#include <X11/Xlib.h>

enum class SpaVideoFormat_e {
  RGBx = SPA_VIDEO_FORMAT_RGBx,
  BGRx = SPA_VIDEO_FORMAT_BGRx,
  RGBA = SPA_VIDEO_FORMAT_RGBA,
  BGRA = SPA_VIDEO_FORMAT_BGRA,
  RGB = SPA_VIDEO_FORMAT_RGB,
  BGR = SPA_VIDEO_FORMAT_BGR,
  INVALID = -1
};

inline std::string spa_to_string(SpaVideoFormat_e const& format){
  switch (format) {
    case SpaVideoFormat_e::RGBx:
      return "RGBx";
    case SpaVideoFormat_e::BGRx:
      return "BGRx";
    case SpaVideoFormat_e::RGBA:
      return "RGBA";
    case SpaVideoFormat_e::BGRA:
      return "BGRA";
    case SpaVideoFormat_e::RGB:
      return "RGB";
    case SpaVideoFormat_e::BGR:
      return "BGR";
    default:
      return "INVALID";
  }
}

inline auto spa_videoformat_bytesize(const SpaVideoFormat_e& format) -> int {
  switch (format) {
    case SpaVideoFormat_e::RGBx:
      return 4;
    case SpaVideoFormat_e::BGRx:
      return 4;
    case SpaVideoFormat_e::RGBA:
      return 4;
    case SpaVideoFormat_e::BGRA:
      return 4;
    case SpaVideoFormat_e::RGB:
      return 3;
    case SpaVideoFormat_e::BGR:
      return 3;
    default:
      return -1;
  }
}

inline auto ximage_to_spa(const XImage& ximage) -> SpaVideoFormat_e {
  if (ximage.format != 2){
    // we only support ZPixmap
    return SpaVideoFormat_e::INVALID;
  }
  if (ximage.bits_per_pixel == 32) {
    // possibly RGBA, BGRA, RGBx or BGRx
    // we just combine RGBx and BGRx to RGBA and BGRA, respectively
    if (ximage.red_mask == 0xff0000 && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff) {
      return SpaVideoFormat_e::BGRA;
    } else if (ximage.red_mask == 0xff && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff0000) {
      return SpaVideoFormat_e::RGBA;
    } else {
      return SpaVideoFormat_e::INVALID;
    }
  } else {
    // possibly RGB or BGR
    if (ximage.red_mask == 0xff0000 && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff) {
      return SpaVideoFormat_e::BGR;
    } else if (ximage.red_mask == 0xff && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff0000) {
      return SpaVideoFormat_e::RGB;
    } else {
      return SpaVideoFormat_e::INVALID;
    }
  }
    
}
