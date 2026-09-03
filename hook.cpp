
#include <cstring>
#include <string>
#include <thread>
#include <tuple>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <X11/Xlib.h>

#include "framebuf.hpp"
#include "payload.hpp"
#include "interface.hpp"
#include "helpers.hpp"
#include "hook.hpp"

/*

  Why I'm using STB here:
  Initially I tried to use opencv as I actually have worked with it before
  However for *some* reason long as the opencv is linked to the hook
  the hook would always crash wemeetapp. I suspect that maybe the wemeetapp
  itself is also using opencv and thereby some conflict arises.

  I also tried the CImg library but its in-memory image layout is just ABSURD,
  which could cause severe performance issue.

  So I eventually picked STB, and I'm happy with it now.

*/

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

constexpr uint32_t DEFAULT_FRAME_HEIGHT = 1080;
constexpr uint32_t DEFAULT_FRAME_WIDTH = 1920;

void XShmAttachHook(){

  auto& interface_singleton = InterfaceSingleton::getSingleton();

  // initialize interface singleton:
  // (1) allocate the interface object
  interface_singleton.interface_handle = new Interface(
    DEFAULT_FB_ALLOC_HEIGHT, DEFAULT_FB_ALLOC_WIDTH,
    DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_WIDTH, SpaVideoFormat_e::RGBA
  );
  // (2) allocate the screencast portal object
  interface_singleton.portal_handle = new XdpScreencastPortal();

  // start the payload thread
  interface_singleton.payload_thread_handle = new std::thread(payload_main);
  fprintf(stderr, "%s", green_text("[hook] payload thread started\n").c_str());

  while(interface_singleton.portal_handle.load()->status.load(std::memory_order_seq_cst) == XdpScreencastPortalStatus::kInit ) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };
  auto payload_status = interface_singleton.portal_handle.load()->status.load(std::memory_order_seq_cst);
  std::string payload_status_str = 
    payload_status == XdpScreencastPortalStatus::kCancelled ? "cancelled" :
    payload_status == XdpScreencastPortalStatus::kRunning ? "running" :
    "unknown";
  if (payload_status == XdpScreencastPortalStatus::kRunning) {
    // things are good
    fprintf(stderr, "%s", green_text("[hook] portal status: " + payload_status_str + "\n").c_str());
  } else {
    // things are bad, we have to de-initialize and exit
    fprintf(stderr, "%s", red_text("[hook] portal status: " + payload_status_str + "\n").c_str());
    fprintf(stderr, "%s", red_text("[hook] <<<!!Please DO NOT cancel screencast!!>> Hook is now exiting.\n").c_str());
    // payload thread should have quitted via g_main_loop_quit
    if (interface_singleton.payload_thread_handle != nullptr && interface_singleton.payload_thread_handle->joinable()) {
      interface_singleton.payload_thread_handle->join();
    }
    delete interface_singleton.payload_thread_handle;
    interface_singleton.payload_thread_handle = nullptr;
    delete interface_singleton.interface_handle.load();
    delete interface_singleton.portal_handle.load();
    interface_singleton.interface_handle.store(nullptr);
    interface_singleton.portal_handle.store(nullptr);
    return;
  }


  while(interface_singleton.portal_handle.load()->pipewire_fd.load(std::memory_order_seq_cst) == -1){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] pipewire_fd acquired: " + std::to_string(interface_singleton.portal_handle.load()->pipewire_fd.load()) + "\n").c_str());

  interface_singleton.pipewire_handle = new PipewireScreenCast(interface_singleton.portal_handle.load()->pipewire_fd.load(), interface_singleton.portal_handle.load()->pipewire_node_ids.at(0));
  fprintf(stderr, "%s", green_text("[hook SYNC] pipewire screencast object allocated\n").c_str());

}

template <typename T>
struct remove_pointer_cvref {
  using type = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
};

template <typename T>
using remove_pointer_cvref_t = typename remove_pointer_cvref<T>::type;


// returns: ximage_width_offset, ximage_height_offset, target_width, target_height
std::tuple<uint32_t, uint32_t, uint32_t, uint32_t> get_resize_param(
  uint32_t ximage_width,
  uint32_t ximage_height,
  uint32_t framebuffer_width,
  uint32_t framebuffer_height
){
  // keep the framebuffer aspect ratio
  double framebuffer_aspect_ratio = static_cast<double>(framebuffer_width) / static_cast<double>(framebuffer_height);
  double ximage_aspect_ratio = static_cast<double>(ximage_width) / static_cast<double>(ximage_height);

  uint32_t target_width = 0;
  uint32_t target_height = 0;
  uint32_t ximage_width_offset = 0;
  uint32_t ximage_height_offset = 0;

  if (framebuffer_aspect_ratio > ximage_aspect_ratio) {
    // framebuffer is wider than ximage
    target_width = ximage_width;
    target_height = (ximage_width * framebuffer_height) / framebuffer_width;
    ximage_height_offset = (ximage_height - target_height) / 2;
  } else {
    // framebuffer is taller than ximage
    target_height = ximage_height;
    target_width = ximage_height * framebuffer_width / framebuffer_height;
    ximage_width_offset = (ximage_width - target_width) / 2;
  }

  return std::make_tuple(ximage_width_offset, ximage_height_offset, target_width, target_height);
}


void XShmGetImageHook(XImage& image){

  auto& interface_singleton = InterfaceSingleton::getSingleton();

  if (interface_singleton.interface_handle.load() == nullptr){
    fprintf(stderr, "%s", red_text("[hook] hook will NOT work as you have cancelled the screencast!!!\n").c_str());
    return;
  }

  auto ximage_spa_format = ximage_to_spa(image);
  auto ximage_width = image.width;
  auto ximage_height = image.height;
  size_t ximage_bytes_per_line = image.bytes_per_line;

  if (ximage_spa_format == SpaVideoFormat_e::INVALID || image.data == nullptr) {
    return;
  }
  std::memset(image.data, 0, ximage_bytes_per_line * ximage_height);

  auto& framebuffer = interface_singleton.interface_handle.load()->framebuf;
  auto framebuffer_spa_format = framebuffer.format;
  auto framebuffer_width = framebuffer.width;
  auto framebuffer_height = framebuffer.height;
  auto framebuffer_row_byte_stride = framebuffer.row_byte_stride;

  const uint8_t* framebuffer_data = framebuffer.data.get();
  if (framebuffer.crop_height && framebuffer.crop_width) {
    framebuffer_data += framebuffer.crop_y * framebuffer_row_byte_stride
      + framebuffer.crop_x * spa_videoformat_bytesize(framebuffer_spa_format);
    framebuffer_width = framebuffer.crop_width;
    framebuffer_height = framebuffer.crop_height;
  }

  // The common XWayland path is 32-bit BGR(A/X) on both sides. Keep this
  // compatibility hook intentionally small; transformed PipeWire frames can
  // be handled separately if they become relevant again.
  if (framebuffer.rotate || framebuffer.flip ||
      spa_videoformat_bytesize(framebuffer_spa_format) != spa_videoformat_bytesize(ximage_spa_format)) {
    return;
  }

  
  // get the resize parameters
  auto [ximage_width_offset, ximage_height_offset, target_width, target_height] = get_resize_param(
    ximage_width, ximage_height, framebuffer_width, framebuffer_height
  );
  auto* output = reinterpret_cast<uint8_t*>(image.data)
    + ximage_height_offset * ximage_bytes_per_line
    + ximage_width_offset * spa_videoformat_bytesize(ximage_spa_format);
  const auto pixel_layout = spa_videoformat_bytesize(ximage_spa_format) == 4
    ? STBIR_4CHANNEL : STBIR_RGB;
  stbir_resize_uint8_srgb(
    framebuffer_data,
    framebuffer_width, framebuffer_height, framebuffer_row_byte_stride,
    output,
    target_width, target_height, ximage_bytes_per_line,
    pixel_layout
  );


  return;
  
}



void XShmDetachStopPWLoop(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  fprintf(stderr, "%s", green_text("[hook] signal pw stop.\n").c_str());
  interface_singleton.interface_handle.load()->pw_stop_flag.store(true, std::memory_order_seq_cst);
  while(!interface_singleton.interface_handle.load()->payload_pw_stop_confirm.load(std::memory_order_seq_cst)){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] pw stop confirmed.\n").c_str());
  return;
}

void XShmDetachStopGIOLoop(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  fprintf(stderr, "%s", green_text("[hook] stop gio main loop.\n").c_str());
  g_main_loop_quit(interface_singleton.portal_handle.load()->gio_mainloop);
  while(!interface_singleton.interface_handle.load()->payload_gio_stop_confirm.load(std::memory_order_seq_cst)){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] gio stop confirmed.\n").c_str());
  return;
}

void XShmDetachClosePortalSession(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  auto* portal_handle = interface_singleton.portal_handle.load();
  if (portal_handle == nullptr) {
    return;
  }

  auto* session = portal_handle->session.load(std::memory_order_seq_cst);
  if (session == nullptr || xdp_session_get_session_state(session) == XDP_SESSION_CLOSED) {
    return;
  }

  // Tear down the portal session before the GIO loop disappears so the
  // backend can fully release the old screencast before the next share starts.
  xdp_session_close(session);
}

void XShmDetachJoinPayloadThread(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  auto* payload_thread = interface_singleton.payload_thread_handle;
  if (payload_thread == nullptr) {
    return;
  }

  if (payload_thread->joinable()) {
    payload_thread->join();
  }

  delete payload_thread;
  interface_singleton.payload_thread_handle = nullptr;
}

void XShmDetachHook(){

  auto& interface_singleton = InterfaceSingleton::getSingleton();
  
  // the interface_handle being non nullptr
  // means that the screencast has been started
  if (interface_singleton.interface_handle != nullptr){

    XShmDetachStopPWLoop();
    XShmDetachClosePortalSession();
    XShmDetachStopGIOLoop();
    XShmDetachJoinPayloadThread();

    // normal de-initialize interface singleton:
    // (1) free the interface object
    delete interface_singleton.interface_handle.exchange(nullptr);
    // (2) free the screencast portal object
    delete interface_singleton.portal_handle.exchange(nullptr);
  } else {
    // we do nothing here since the objects (interface, portal) are already freed,
    // and pipewire object has never been created
    fprintf(stderr, "%s", red_text("[hook] objects are already freed because of cancelled screencast. exiting.\n").c_str());
  }
  

}

extern "C" {

Bool XShmAttach(Display* dpy, XShmSegmentInfo* shminfo){
  XShmAttachHook();
  return XShmAttachFunc(dpy, shminfo);
}

Bool XShmGetImage(Display* dpy, Drawable d, XImage* image, int x, int y, unsigned long plane_mask){
  XShmGetImageHook(*image);
  return 1;
}

Bool XShmDetach(Display* dpy, XShmSegmentInfo* shminfo){
  XShmDetachHook();
  return XShmDetachFunc(dpy, shminfo);
}

Bool XDamageQueryExtension(Display *dpy, int *event_base_return, int *error_base_return) {
  return 0;
}

}
