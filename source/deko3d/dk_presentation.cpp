#include "dk_gpu.h"
#include "../saltynx.h"
#include <algorithm>

namespace nxdk {
Image *Gpu::backbuffer() {
  if (!m_swapchain) return nullptr;
  if (m_slot < 0) m_slot = m_queue.acquireImage(m_swapchain);
  if (m_slot < 0 || m_slot >= int(m_backbuffers.size())) return nullptr;
  return m_backbuffers[m_slot].get();
}

void Gpu::present() {
  Image *frame = backbuffer();
  if (!frame) return;
  const GeneratedFrame generated = lsfg_generate(*frame);
  if (generated.image) {
    m_swapchain.setSwapInterval(1);
    present_image(*generated.image);
    present_image(*generated.source);
    m_swapchain.setSwapInterval(m_swap_interval);
  } else {
    present_backbuffer();
  }
  saltynx_frame(1);
}

void Gpu::present_image(const Image &image) {
  Image *target = backbuffer();
  if (!target) return;
  const DkImageView source = image.view(), destination = target->view();
  const DkImageRect rect{0, 0, 0, image.width, image.height, 1};
  barrier(DkBarrier_Full, DkInvalidateFlags_Image);
  dkCmdBufBlitImage(commands(), &source, &rect, &destination, &rect, 0, 0);
  image_written(target);
  present_backbuffer();
}

void Gpu::present_backbuffer() {
  const int slot = m_slot;
  if (slot < 0) return;
  submit(!m_presented);
  m_queue.presentImage(m_swapchain, slot);
  m_presented = true;
  m_slot = -1;
}

void Gpu::set_swap_interval(int interval) {
  m_swap_interval = std::max(0, interval);
  if (m_swapchain) m_swapchain.setSwapInterval(m_swap_interval);
}
}
