/*!
  \file ldr_image.hpp
  \author Sho Ikeda

  Copyright (c) 2015-2017 Sho Ikeda
  This software is released under the MIT License.
  http://opensource.org/licenses/mit-license.php
  */

#ifndef NANAIRO_LDR_IMAGE_HPP
#define NANAIRO_LDR_IMAGE_HPP

// Standard C++ library
#include <array>
#include <cstddef>
#include <vector>
// Nanairo
#include "rgba_32.hpp"
#include "NanairoCore/nanairo_core_config.hpp"

namespace nanairo {

//! \addtogroup Core
//! \{

/*!
  */
class LdrImage
{
 public:
  //! Initialize as black image
  LdrImage(const uint width, const uint height) noexcept;


  //! Return the RGBA by the index
  Rgba32& operator[](const uint index) noexcept;

  //! Return the RGBA by the index
  const Rgba32& operator[](const uint index) const noexcept;


  //! Return the buffer data
  std::vector<Rgba32>& data() noexcept;

  //! Return the buffer data
  const std::vector<Rgba32>& data() const noexcept;

  //! Fill the image by the color
  void fill(const Rgba32 color) noexcept;

  //! Return the RGBA by the index
  Rgba32& get(const uint index) noexcept;

  //! Return the RGBA by the index
  const Rgba32& get(const uint index) const noexcept;

  //! Return the RGBA by the index
  Rgba32& get(const uint x, const uint y) noexcept;

  //! Return the RGBA by the index
  const Rgba32& get(const uint x, const uint y) const noexcept;

  //! Return the height resolution
  uint heightResolution() const noexcept;

  //! Return the buffer memory usage 
  std::size_t memoryUsage() const noexcept;

  //! Return the image resolution
  const std::array<uint, 2>& resolution() const noexcept;

  //! Return the num of pixels 
  uint size() const noexcept;

  //! Set the image resolution and initialize image as black image
  void setResolution(const uint width, const uint height) noexcept;

  //! Return the width resolution
  uint widthResolution() const noexcept;

 private:
  //! Convert a XY coordinate to a pixel index
  uint toIndex(const uint x, const uint y) const noexcept;


  std::vector<Rgba32> buffer_;
  std::array<uint, 2> resolution_;
};

//! \} Core

} // namespace nanairo

#include "ldr_image-inl.hpp"

#endif // NANAIRO_LDR_IMAGE_HPP
