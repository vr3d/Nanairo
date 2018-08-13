/*!
  \file bayesian_collaborative_denoiser.cpp
  \author Sho Ikeda

  Copyright (c) 2015-2018 Sho Ikeda
  This software is released under the MIT License.
  http://opensource.org/licenses/mit-license.php
  */

// Standard C++ library
#include <algorithm>
#include <array>
#include <bitset>
#include <limits>
#include <vector>
// Zisc
#include "zisc/arith_array.hpp"
#include "zisc/error.hpp"
#include "zisc/math.hpp"
#include "zisc/memory_resource.hpp"
#include "zisc/utility.hpp"
// Nanairo
#include "bayesian_collaborative_denoiser.hpp"
#include "denoiser.hpp"
#include "NanairoCore/nanairo_core_config.hpp"
#include "NanairoCore/system.hpp"
#include "NanairoCore/Color/SpectralDistribution/spectral_distribution.hpp"
#include "NanairoCore/Data/rendering_tile.hpp"
#include "NanairoCore/Sampling/sample_statistics.hpp"
#include "NanairoCore/Setting/system_setting_node.hpp"
#include "NanairoCore/Setting/setting_node_base.hpp"

namespace nanairo {

/*!
  */
BayesianCollaborativeDenoiser::BayesianCollaborativeDenoiser(
    const SettingNodeBase* settings) noexcept :
        Denoiser(settings)
{
  initialize(settings);
}

/*!
  */
void BayesianCollaborativeDenoiser::denoise(
    System& system,
    const uint32 cycle,
    SampleStatistics* statistics) const noexcept
{
  if (system.colorMode() == RenderingColorMode::kRgb)
     denoiseMultiscale<3>(system, cycle, statistics);
  else
     denoiseMultiscale<CoreConfig::spectraSize()>(system, cycle, statistics);
}

/*!
  */
template <uint kDimension>
BayesianCollaborativeDenoiser::Parameters<kDimension>::Parameters(System& system)
    noexcept :
        sample_value_table_{&system.globalMemoryManager()},
        histogram_table_{&system.globalMemoryManager()},
        covariance_factor_table_{&system.globalMemoryManager()},
        denoised_value_table_{&system.globalMemoryManager()}
{
}

/*!
  */
template <uint kDimension> template <uint kN>
void BayesianCollaborativeDenoiser::Parameters<kDimension>::upscaleAdd(
    const Index2d& low_res,
    const zisc::pmr::vector<zisc::ArithArray<Float, kN>>& low_res_table,
    const Index2d& high_res,
    zisc::pmr::vector<zisc::ArithArray<Float, kN>>* high_res_table,
    const Index2d& range) noexcept
{
  for (uint high_index = range[0]; high_index < range[1]; ++high_index) {
    const Index2d high_pixel{high_index % high_res[0], high_index / high_res[0]};

    const Index2d low_p1{high_pixel[0] >> 1, high_pixel[1] >> 1};
    Index2d low_p2 = low_p1;
    for (uint i = 0; i < low_p2.size(); ++i) {
      low_p2[i] = zisc::isOdd(high_pixel[i]) ? low_p2[i] + 1 :
                  (0 < low_p2[i])            ? low_p2[i] - 1
                                             : 0;
      low_p2[i] = zisc::min(low_p2[i], low_res[i] - 1);
    }

    constexpr Float main_weight = 9.0 / 16.0;
    constexpr Float adjacent_weight = 3.0 / 16.0;
    constexpr Float diagonal_weight = 1.0 / 16.0;
    auto interp = main_weight * low_res_table[low_p1[0] + low_res[0] * low_p1[1]];
    interp += adjacent_weight * low_res_table[low_p1[0] + low_res[0] * low_p2[1]];
    interp += adjacent_weight * low_res_table[low_p2[0] + low_res[0] * low_p1[1]];
    interp += diagonal_weight * low_res_table[low_p2[0] + low_res[0] * low_p2[1]];
    (*high_res_table)[high_index] += interp;
  }
}

/*!
  */
template <uint kDimension> template <uint kN>
void BayesianCollaborativeDenoiser::Parameters<kDimension>::downscaleAverage(
    const Index2d& high_res,
    const zisc::pmr::vector<zisc::ArithArray<Float, kN>>& high_res_table,
    const Index2d& low_res,
    zisc::pmr::vector<zisc::ArithArray<Float, kN>>* low_res_table,
    const Index2d& range) noexcept
{
  for (uint low_index = range[0]; low_index < range[1]; ++low_index) {
    const Index2d low_pixel{low_index % low_res[0], low_index / low_res[0]};
    zisc::ArithArray<Float, kN> sum;
    for (uint offset_y = 0; offset_y < 2; ++offset_y) {
      for (uint offset_x = 0; offset_x < 2; ++offset_x) {
        const Index2d high_pixel{
            zisc::min(2 * low_pixel[0] + offset_x, high_res[0] - 1),
            zisc::min(2 * low_pixel[1] + offset_y, high_res[1] - 1)};
        const uint high_index = high_pixel[0] + high_res[0] * high_pixel[1];
        sum += high_res_table[high_index];
      }
    }
    (*low_res_table)[low_index] = 0.25 * sum;
  }
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::Parameters<kDimension>::downscaleOf(
    System& system,
    const Parameters& high_res_p) noexcept
{
  for (uint i = 0; i < resolution_.size(); ++i)
    resolution_[i] = high_res_p.resolution_[i] >> 1;
  num_of_samples_ = high_res_p.num_of_samples_;
  histogram_bins_ = high_res_p.histogram_bins_;

  sample_value_table_.resize(resolution_[0] * resolution_[1]);
  histogram_table_.resize(histogram_bins_ * resolution_[0] * resolution_[1]);
  covariance_factor_table_.resize(resolution_[0] * resolution_[1]);
  denoised_value_table_.resize(resolution_[0] * resolution_[1]);

  auto downscale_parameters = [this, &system, &high_res_p]
  (const int thread_id, const uint)
  {
    // Set the calculation range
    const auto range = system.calcThreadRange(resolution_[0] * resolution_[1],
                                              thread_id);
    downscaleAverage(high_res_p.resolution_, high_res_p.sample_value_table_,
                     resolution_, &sample_value_table_,
                     Index2d{range[0], range[1]});
    for (uint b = 0; b < histogram_bins_; ++b) {
      const uint offset = b * (resolution_[0] * resolution_[1]);
      downscaleAverage(high_res_p.resolution_, high_res_p.histogram_table_,
                       resolution_, &histogram_table_,
                       Index2d{offset + range[0], offset + range[1]});
    }
    downscaleAverage(high_res_p.resolution_, high_res_p.covariance_factor_table_,
                     resolution_, &covariance_factor_table_,
                     Index2d{range[0], range[1]});
  };

  {
    auto& threads = system.threadManager();
    auto& work_resource = system.globalMemoryManager();
    constexpr uint start = 0;
    const uint end = threads.numOfThreads();
//    auto result = threads.enqueueLoop(init_parameters, start, end, &work_resource);
//    result.wait();
#pragma omp parallel for
    for (uint thread_id = start; thread_id < end; ++thread_id)
      downscale_parameters(zisc::cast<int>(thread_id), 0);
  }
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::Parameters<kDimension>::init(
    System& system,
    const uint32 cycle,
    const uint histogram_bins,
    const SampleStatistics& statistics) noexcept
{
  resolution_ = system.imageResolution();
  num_of_samples_ = cycle;
  histogram_bins_ = histogram_bins;

  sample_value_table_.resize(resolution_[0] * resolution_[1]);
  histogram_table_.resize(histogram_bins_ * resolution_[0] * resolution_[1]);
  covariance_factor_table_.resize(resolution_[0] * resolution_[1]);
  denoised_value_table_.resize(resolution_[0] * resolution_[1]);

  auto init_parameters = [this, &system, &statistics]
  (const int thread_id, const uint)
  {
    // Set the calculation range
    const auto range = system.calcThreadRange(resolution_[0] * resolution_[1],
                                              thread_id);

    const Float k = zisc::invert(zisc::cast<Float>(num_of_samples_));
    const Float k1 = zisc::invert(zisc::cast<Float>(num_of_samples_ - 1));
    for (uint pixel_index = range[0]; pixel_index < range[1]; ++pixel_index) {
      const auto& sample_p = statistics.sampleTable()[pixel_index];
      // Calculate expected values
      {
        auto& sample_value = sample_value_table_[pixel_index];
        for (uint si = 0; si < kDimension; ++si)
          sample_value[si] = k * sample_p->get(si);
      }
      // Calculate covariance matrix
      {
        const auto& sample_squared_p = statistics.sampleSquaredTable()[pixel_index];
        const uint factor_index = statistics.numOfCovarianceFactors() * pixel_index;
        const auto factors = &statistics.covarianceFactorTable()[factor_index];
        auto& covariance_factor = covariance_factor_table_[pixel_index];
        for (uint offset = 0, si_a = 0; si_a < kDimension; ++si_a) {
          for (uint si_b = si_a; si_b < kDimension; ++si_b, ++offset) {
            covariance_factor[offset] = (si_a == si_b)
                ? sample_squared_p->get(si_a)
                : factors[statistics.getFactorIndex(si_a) + ((si_b - si_a) - 1)].get();
            covariance_factor[offset] -= k * sample_p->get(si_a) * sample_p->get(si_b);
          }
        }
        covariance_factor *= k * k1;
      }
    }
    // Calculate histogram
    for (uint b = 0; b < histogram_bins_; ++b) {
      const uint histogram_offset = b * (resolution_[0] * resolution_[1]);
      for (uint pixel_index = range[0]; pixel_index < range[1]; ++pixel_index) {
        const uint src_index = histogram_bins_ * pixel_index + b;
        const auto& src = statistics.histogramTable()[src_index];
        const uint dst_index = histogram_offset + pixel_index;
        auto& dst = histogram_table_[dst_index];
        for (uint si = 0; si < kDimension; ++si)
          dst[si] = src->get(si);
      }
    }
  };

  {
    auto& threads = system.threadManager();
    auto& work_resource = system.globalMemoryManager();
    constexpr uint start = 0;
    const uint end = threads.numOfThreads();
//    auto result = threads.enqueueLoop(init_parameters, start, end, &work_resource);
//    result.wait();
#pragma omp parallel for
    for (uint thread_id = start; thread_id < end; ++thread_id)
      init_parameters(zisc::cast<int>(thread_id), 0);
  }
}

/*!
  */
BayesianCollaborativeDenoiser::PixelMarker::PixelMarker(System& system) noexcept :
    marker_table_{&system.globalMemoryManager()}
{
  constexpr uint marker_bytes = zisc::power<kMarkerRepBits>(2) / 8;
  static_assert(sizeof(Marker) == marker_bytes, "The size of marker is wrong.");
  const auto& resolution = system.imageResolution();
  const uint num_of_marks = (resolution[0] * resolution[1]) >> kMarkerRepBits;
  marker_table_.resize(num_of_marks);
}

/*!
  */
void BayesianCollaborativeDenoiser::PixelMarker::clear() noexcept
{
  for (auto& marker : marker_table_)
    marker.reset();
}

/*!
  */
bool BayesianCollaborativeDenoiser::PixelMarker::isMarked(const uint index)
    const noexcept
{
  constexpr uint mask = sizeof(Marker) * 8 - 1;
  const uint marker_index = index >> kMarkerRepBits;
  const uint i = index & mask;
  return marker_table_[marker_index][i];
}

/*!
  */
void BayesianCollaborativeDenoiser::PixelMarker::mark(const uint index) noexcept
{
  constexpr uint mask = sizeof(Marker) * 8 - 1;
  const uint marker_index = index >> kMarkerRepBits;
  const uint i = index & mask;
  marker_table_[marker_index].set(i);
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::aggregate(
    System& system,
    const zisc::pmr::vector<int>& estimates_counter,
    Parameters<kDimension>* parameters) const noexcept
{
  auto aggregate_values = [&system, &estimates_counter, parameters]
  (const int thread_id, const uint)
  {
    // Set the calculation range
    const auto& resolution = parameters->resolution_;
    const auto range = system.calcThreadRange(resolution[0] * resolution[1],
                                              thread_id);

    for (uint pixel_index = range[0]; pixel_index < range[1]; ++pixel_index) {
      ZISC_ASSERT(0 < estimates_counter[pixel_index], "The estimate count is zero.");
      auto& target = parameters->denoised_value_table_[pixel_index];
      target *= zisc::invert(zisc::cast<Float>(estimates_counter[pixel_index]));
    }
  };

  {
    auto& threads = system.threadManager();
    auto& work_resource = system.globalMemoryManager();
    constexpr uint start = 0;
    const uint end = threads.numOfThreads();
//    auto result = threads.enqueueLoop(aggregate_values, start, end, &work_resource);
//    result.wait();
#pragma omp parallel for
    for (uint thread_id = start; thread_id < end; ++thread_id)
      aggregate_values(zisc::cast<int>(thread_id), 0);
  }
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::aggregateFinal(
    System& system,
    const Parameters<kDimension>& parameters,
    SampleStatistics* statistics) const noexcept
{
  auto aggregate_values = [&system, &parameters, statistics]
  (const int thread_id, const uint)
  {
    // Set the calculation range
    const auto& resolution = parameters.resolution_;
    const auto range = system.calcThreadRange(resolution[0] * resolution[1],
                                              thread_id);

    for (uint pixel_index = range[0]; pixel_index < range[1]; ++pixel_index) {
      const Index2d p{pixel_index % resolution[0],
                      pixel_index / resolution[0]};
      const auto& src = parameters.denoised_value_table_[pixel_index];
      const uint dst_index = p[0] + statistics->resolution()[0] * p[1];
      auto& dst = statistics->denoisedSampleTable()[dst_index];
      for (uint si = 0; si < kDimension; ++si)
        dst->set(si, src[si]);
    }
  };

  {
    auto& threads = system.threadManager();
    auto& work_resource = system.globalMemoryManager();
    constexpr uint start = 0;
    const uint end = threads.numOfThreads();
//    auto result = threads.enqueueLoop(aggregate_values, start, end, &work_resource);
//    result.wait();
#pragma omp parallel for
    for (uint thread_id = start; thread_id < end; ++thread_id)
      aggregate_values(zisc::cast<int>(thread_id), 0);
  }
}

/*!
  */
template <uint kN>
zisc::ArithArray<Float, kN> BayesianCollaborativeDenoiser::calcEmpiricalMean(
    const Index2d& resolution,
    RenderingTile& search_window,
    const Index2d& patch_offset,
    const SimilarPatchMask& similar_mask,
    const zisc::ArithArray<Float, kN>* table) const noexcept
{
  search_window.reset();
  zisc::ArithArray<Float, kN> mean;
  for (uint p = 0; p < search_window.numOfPixels(); ++p) {
    const auto& neighbor_pixel = search_window.current();
    if (similar_mask[search_window.getIndex(neighbor_pixel)]) {
      const Index2d src_pixel{(neighbor_pixel[0] + patch_offset[0]) - patch_radius_,
                              (neighbor_pixel[1] + patch_offset[1]) - patch_radius_};
      const uint src_index = src_pixel[0] + resolution[0] * src_pixel[1];
      mean += table[src_index];
    }
    search_window.next();
  }
  mean *= zisc::invert(zisc::cast<Float>(similar_mask.count()));
  return mean;
}

/*!
  */
template <uint kDimension>
auto BayesianCollaborativeDenoiser::calcEmpiricalCovarianceMatrix(
    const Index2d& resolution,
    RenderingTile& search_window,
    const Index2d& patch_offset,
    const SimilarPatchMask& similar_mask,
    const zisc::pmr::vector<SpectraArray<kDimension>>& value_table,
    const SpectraArray<kDimension>& value_mean) const noexcept
        -> CovarianceFactors<kDimension>
{
  search_window.reset();
  CovarianceFactors<kDimension> mean;
  for (uint p = 0; p < search_window.numOfPixels(); ++p) {
    const auto& neighbor_pixel = search_window.current();
    if (similar_mask[search_window.getIndex(neighbor_pixel)]) {
      const Index2d src_pixel{(neighbor_pixel[0] + patch_offset[0]) - patch_radius_,
                              (neighbor_pixel[1] + patch_offset[1]) - patch_radius_};
      const uint src_index = src_pixel[0] + resolution[0] * src_pixel[1];

      const auto diff = value_table[src_index] - value_mean;
      for (uint offset = 0, si_a = 0; si_a < diff.size(); ++si_a) {
        for (uint si_b = si_a; si_b < diff.size(); ++si_b, ++offset)
          mean[offset] += diff[si_a] * diff[si_b];
      }
    }
    search_window.next();
  }
  mean *= zisc::invert(zisc::cast<Float>(similar_mask.count() - 1));
  return mean;
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::calcStagingDenoisedValue(
    const Index2d& resolution,
    RenderingTile& search_window,
    const Index2d& patch_offset,
    const SimilarPatchMask& similar_mask,
    const SpectraArray<kDimension>& expected_mean,
    const Matrix<kDimension>& covariance_mean,
    const Matrix<kDimension>& expected_covariance,
    const zisc::pmr::vector<SpectraArray<kDimension>>& value_table,
    zisc::pmr::vector<SpectraArray<kDimension>>* staging_value_table) const noexcept
{
  const auto inv_expected_covariance = expected_covariance.inverseMatrix();
  search_window.reset();
  for (uint p = 0; p < search_window.numOfPixels(); ++p) {
    const auto& neighbor_pixel = search_window.current();
    if (similar_mask[search_window.getIndex(neighbor_pixel)]) {
      const Index2d target_pixel{(neighbor_pixel[0] + patch_offset[0]) -
                                 patch_radius_,
                                 (neighbor_pixel[1] + patch_offset[1]) -
                                 patch_radius_};
      const uint index = target_pixel[0] + resolution[0] * target_pixel[1];

      const auto& x = value_table[index];
      const auto tmp = inv_expected_covariance * (x - expected_mean);
      (*staging_value_table)[index] = x - (covariance_mean * tmp);
    }
    search_window.next();
  }
}

/*!
  */
template <uint kDimension>
Float BayesianCollaborativeDenoiser::calcHistogramDistance(
    const SpectraArray<kDimension>& histogram_lhs,
    const SpectraArray<kDimension>& histogram_rhs,
    uint* num_of_non_both0) const noexcept
{
  ZISC_ASSERT(num_of_non_both0 != nullptr, "The num_of_non_both0 is null.");
  Float distance_sum = 0.0;
  for (uint si = 0; si < kDimension; ++si) {
    const Float lhs = histogram_lhs[si];
    const Float rhs = histogram_rhs[si];
    if (1.0 < (lhs + rhs)) {
      ++(*num_of_non_both0);
      const Float d = zisc::power<2>(lhs - rhs) / (lhs + rhs);
      distance_sum += d;
    }
  }
  return distance_sum;
}

/*!
  */
template <uint kDimension>
Float BayesianCollaborativeDenoiser::calcHistogramPatchDistance(
    const Parameters<kDimension>& parameters,
    const Index2d& center_pixel_lhs,
    const Index2d& center_pixel_rhs) const noexcept
{
  const auto& resolution = parameters.resolution_;
  Float histogram_distance = 0.0;
  uint num_of_non_both0 = 0;

  auto patch_lhs = makePatch(center_pixel_lhs);
  auto patch_rhs = makePatch(center_pixel_rhs);

  for (uint b = 0; b < parameters.histogram_bins_; ++b) {
    const uint histogram_offset = b * resolution[0] * resolution[1];

    patch_lhs.reset();
    patch_rhs.reset();
    for (uint p = 0; p < getNumOfPatchPixels(); ++p) {
      const auto& p_lhs = patch_lhs.current();
      const auto& p_rhs = patch_rhs.current();

      const uint index_lhs = p_lhs[0] + resolution[0] * p_lhs[1];
      const uint index_rhs = p_rhs[0] + resolution[0] * p_rhs[1];

      const auto& histogram_table = parameters.histogram_table_;
      const auto& histogram_lhs = histogram_table[histogram_offset + index_lhs];
      const auto& histogram_rhs = histogram_table[histogram_offset + index_rhs];
      histogram_distance += calcHistogramDistance(histogram_lhs,  
                                                  histogram_rhs,
                                                  &num_of_non_both0);

      patch_lhs.next();
      patch_rhs.next();
    }
  }
  ZISC_ASSERT(0 < num_of_non_both0, "The num of elements is zero.");
  histogram_distance = histogram_distance / zisc::cast<Float>(num_of_non_both0);
  return histogram_distance;
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::denoiseChunk(
    System& system,
    const Index2d& chunk_resolution,
    const Index2d& tile_position,
    Parameters<kDimension>* parameters,
    zisc::pmr::vector<SpectraArray<kDimension>>* staging_value_table,
    zisc::pmr::vector<int>* estimates_counter,
    PixelMarker* pixel_marker) const noexcept
{
  auto denoise_chunk =
  [this, parameters, staging_value_table, estimates_counter, pixel_marker,
   chunk_resolution, tile_position]
  (const uint chunk_number)
  {
    const auto& resolution = parameters->resolution_;
    const Index2d chunk_position{chunk_number % chunk_resolution[0],
                                 chunk_number / chunk_resolution[0]};
    auto chunk_tile = makeChunkTile(resolution, chunk_position, tile_position);
    for (uint p = 0; p < chunk_tile.numOfPixels(); ++p) {
      const auto& current_pixel = chunk_tile.current();
      const uint pixel_index = current_pixel[0] +
                               resolution[0] * current_pixel[1];
      if (!pixel_marker->isMarked(pixel_index)) {
        denoisePixels(current_pixel, parameters,
                      staging_value_table, estimates_counter, pixel_marker);
      }
      chunk_tile.next();
    }
  };

  {
    auto& threads = system.threadManager();
    auto& work_resource = system.globalMemoryManager();
    constexpr uint start = 0;
    const uint end = chunk_resolution[0] * chunk_resolution[1];
//    auto result = threads.enqueueLoop(denoise_chunk, start, end, &work_resource);
//    result.wait();
#pragma omp parallel for
    for (uint thread_id = start; thread_id < end; ++thread_id)
      denoise_chunk(thread_id);
  }
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::denoiseMultiscale(
    System& system,
    const uint32 cycle,
    SampleStatistics* statistics) const noexcept
{
  zisc::pmr::vector<Parameters<kDimension>> multiscale_parameters{
      &system.globalMemoryManager()};
  multiscale_parameters.reserve(num_of_scales_);
  for (uint scale = 0; scale < num_of_scales_; ++scale)
    multiscale_parameters.emplace_back(system);
  multiscale_parameters[0].init(system, cycle, histogramBins(), *statistics);
  for (uint scale = 1; scale < num_of_scales_; ++scale)
    multiscale_parameters[scale].downscaleOf(system, multiscale_parameters[scale - 1]);

  zisc::pmr::vector<SpectraArray<kDimension>> staging_value_table{
      &system.globalMemoryManager()};
  staging_value_table.resize(multiscale_parameters[0].sample_value_table_.size());

  zisc::pmr::vector<int> estimates_counter{&system.globalMemoryManager()};
  estimates_counter.resize(multiscale_parameters[0].sample_value_table_.size());

  PixelMarker pixel_marker{system};

  Parameters<kDimension>* parameters = nullptr;
  for (uint iteration = 0; iteration < num_of_scales_; ++iteration) {
    // Clear buffers
    std::fill(estimates_counter.begin(), estimates_counter.end(), 0);
    pixel_marker.clear();

    const uint scale = num_of_scales_ - (iteration + 1);
    parameters = &multiscale_parameters[scale];

    const Index2d chunk_resolution = getChunkResolution(parameters->resolution_);
    constexpr auto tile_order = getChunkTileOrder();
    for (uint tile_number = 0; tile_number < tile_order.size(); ++tile_number) {
      const auto tile_position = tile_order[tile_number];
      denoiseChunk(system, chunk_resolution, tile_position, parameters,
                   &staging_value_table, &estimates_counter, &pixel_marker);
      std::cout << "scale: " << scale << ", tile[" << tile_number << "]" << std::endl;
    }
    aggregate(system, estimates_counter, parameters);
//    if (0 < iteration) {
//      auto& low_res_p = multiscale_parameters[scale_level + 1];
//      merge(system, &low_res_p, &parameters, &staging_value_table);
//    }
    break;
  }
  aggregateFinal(system, *parameters, statistics);
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::denoisePixels(
    const Index2d& main_pixel,
    Parameters<kDimension>* parameters,
    zisc::pmr::vector<SpectraArray<kDimension>>* staging_value_table,
    zisc::pmr::vector<int>* estimates_counter,
    PixelMarker* pixel_marker) const noexcept
{
  const auto similar_mask = selectSimilarPatches(*parameters, main_pixel);
  const uint num_of_similar_patches = zisc::cast<uint>(similar_mask.count());
  if (num_of_similar_patches <= getPatchDimension<kDimension>()) {
    denoiseOnlyMainPatch(main_pixel, similar_mask, parameters, estimates_counter);
  }
  else {
    denoiseSelectedPatches(main_pixel, similar_mask, parameters,
                           staging_value_table, estimates_counter, pixel_marker);
  }
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::denoiseOnlyMainPatch(
    const Index2d& main_pixel,
    const SimilarPatchMask& similar_mask,
    Parameters<kDimension>* parameters,
    zisc::pmr::vector<int>* estimates_counter) const noexcept
{
  const auto& resolution = parameters->resolution_;
  auto search_window = makeSearchWindow(resolution, main_pixel);

  for (uint patch_number = 0; patch_number < getNumOfPatchPixels(); ++patch_number) {
    const Index2d patch_offset{patch_number % (2 * patch_radius_ + 1),
                               patch_number / (2 * patch_radius_ + 1)};
    // Sum up sample values of similar pixels
    SpectraArray<kDimension> estimated_value;
    search_window.reset();
    for (uint p = 0; p < search_window.numOfPixels(); ++p) {
      const auto& neighbor_pixel = search_window.current();
      if (similar_mask[search_window.getIndex(neighbor_pixel)]) {
        const Index2d src_pixel{(neighbor_pixel[0] + patch_offset[0]) -
                                patch_radius_,
                                (neighbor_pixel[1] + patch_offset[1]) -
                                patch_radius_};
        const uint src_index = src_pixel[0] + resolution[0] * src_pixel[1];
        estimated_value += parameters->sample_value_table_[src_index];
      }
      search_window.next();
    }
    estimated_value *= zisc::invert(zisc::cast<Float>(similar_mask.count()));
    // Calc estimated value
    {
      const Index2d dst_pixel{(main_pixel[0] + patch_offset[0]) - patch_radius_,
                              (main_pixel[1] + patch_offset[1]) - patch_radius_};
      const uint dst_index = dst_pixel[0] + resolution[0] * dst_pixel[1];
      parameters->denoised_value_table_[dst_index] += estimated_value;
      ++(*estimates_counter)[dst_index];
    }
  }
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::denoiseSelectedPatches(
    const Index2d& main_pixel,
    const SimilarPatchMask& similar_mask,
    Parameters<kDimension>* parameters,
    zisc::pmr::vector<SpectraArray<kDimension>>* staging_value_table,
    zisc::pmr::vector<int>* estimates_counter,
    PixelMarker* pixel_marker) const noexcept
{
  const auto& resolution = parameters->resolution_;
  auto search_window = makeSearchWindow(resolution, main_pixel);

  for (uint patch_number = 0; patch_number < getNumOfPatchPixels(); ++patch_number) {
    const Index2d patch_offset{patch_number % (2 * patch_radius_ + 1),
                               patch_number / (2 * patch_radius_ + 1)};

    // Step1
    const auto covariance_mean = toMatrix<kDimension>(calcEmpiricalMean(
        resolution, search_window, patch_offset, similar_mask,
        parameters->covariance_factor_table_.data()));
    auto expected_mean = calcEmpiricalMean(
        resolution, search_window, patch_offset, similar_mask,
        parameters->sample_value_table_.data());
    auto expected_covariance = toMatrix<kDimension>(calcEmpiricalCovarianceMatrix(
        resolution, search_window, patch_offset, similar_mask,
        parameters->sample_value_table_, expected_mean));
    //! \todo Clamping
    expected_covariance = covariance_mean + (expected_covariance - covariance_mean);
    calcStagingDenoisedValue(
        resolution, search_window, patch_offset, similar_mask,
        expected_mean, covariance_mean, expected_covariance,
        parameters->sample_value_table_, staging_value_table);
    // Step2
    expected_mean = calcEmpiricalMean(
        resolution, search_window, patch_offset, similar_mask,
        staging_value_table->data());
    expected_covariance = toMatrix<kDimension>(calcEmpiricalCovarianceMatrix(
        resolution, search_window, patch_offset, similar_mask,
        *staging_value_table, expected_mean));
    calcStagingDenoisedValue(
        resolution, search_window, patch_offset, similar_mask,
        expected_mean, covariance_mean, expected_covariance + covariance_mean,
        parameters->sample_value_table_, staging_value_table);

    search_window.reset();
    for (uint p = 0; p < search_window.numOfPixels(); ++p) {
      const auto& neighbor_pixel = search_window.current();
      if (similar_mask[search_window.getIndex(neighbor_pixel)]) {
        const Index2d target_pixel{(neighbor_pixel[0] + patch_offset[0]) -
                                   patch_radius_,
                                   (neighbor_pixel[1] + patch_offset[1]) -
                                   patch_radius_};
        const uint index = target_pixel[0] + resolution[0] * target_pixel[1];

        const auto& staging_value = (*staging_value_table)[index];
        parameters->denoised_value_table_[index] += staging_value;
        ++(*estimates_counter)[index];
        if ((patch_offset.data() == Index2d{patch_radius_, patch_radius_}.data()))
          pixel_marker->mark(index);
      }
      search_window.next();
    }
  }
}

/*!
  */
Index2d BayesianCollaborativeDenoiser::getChunkResolution(Index2d resolution)
    const noexcept
{
  resolution[0] = resolution[0] - 2 * patch_radius_;
  resolution[1] = resolution[1] - 2 * patch_radius_;

  const uint chunk_size = getChunkSize();
  const uint chunk_w = (resolution[0] / chunk_size) + 
      (((resolution[0] % chunk_size) != 0) ? 1 : 0);
  const uint chunk_h = (resolution[1] / chunk_size) + 
      (((resolution[1] % chunk_size) != 0) ? 1 : 0);

  return Index2d{chunk_w, chunk_h};
}

/*!
  */
uint BayesianCollaborativeDenoiser::getChunkSize() const noexcept
{
  const uint size = 3 * search_radius_;
  return size;
}

/*!
  */
uint BayesianCollaborativeDenoiser::getNumOfPatchPixels() const noexcept
{
  const uint size = zisc::power<2>(2 * patch_radius_ + 1);
  return size;
}

/*!
  */
uint BayesianCollaborativeDenoiser::getNumOfSearchWindowPixels() const noexcept
{
  const uint max_num_of_patches = zisc::power<2>(2 * search_radius_ + 1);
  return max_num_of_patches;
}

/*!
  */
template <uint kDimension>
uint BayesianCollaborativeDenoiser::getPatchDimension() const noexcept
{
  const uint dimension = kDimension * getNumOfPatchPixels();
  return dimension;
}

/*!
  */
void BayesianCollaborativeDenoiser::initialize(const SettingNodeBase* settings) noexcept
{
  const auto system_settings = castNode<SystemSettingNode>(settings);
  const auto& parameters = system_settings->bayesianCollaborativeDenoiserParameters();
  histogram_bins_ =
      zisc::cast<uint>(parameters.histogram_bins_);
  histogram_distance_threshold_ = 
      zisc::cast<Float>(parameters.histogram_distance_threshold_);
  patch_radius_ =
      zisc::cast<uint>(parameters.patch_radius_);
  search_radius_ =
      zisc::cast<uint>(parameters.search_window_radius_);
  num_of_scales_ =
      zisc::cast<uint>(parameters.number_of_scales_);
}

/*!
  */
RenderingTile BayesianCollaborativeDenoiser::makePatch(const Index2d& center_pixel)
    const noexcept
{
  Index2d begin{center_pixel[0] - patch_radius_,
                center_pixel[1] - patch_radius_};
  Index2d end{center_pixel[0] + patch_radius_ + 1,
              center_pixel[1] + patch_radius_ + 1};
  return RenderingTile{begin, end};
}

/*!
  */
RenderingTile BayesianCollaborativeDenoiser::makeSearchWindow(
    const Index2d& resolution,
    const Index2d& center_pixel) const noexcept
{
  Index2d begin{
      zisc::max(patch_radius_ + search_radius_, center_pixel[0]) - search_radius_,
      zisc::max(patch_radius_ + search_radius_, center_pixel[1]) - search_radius_};
  Index2d end{
      zisc::min(resolution[0] - patch_radius_, center_pixel[0] + search_radius_+1),
      zisc::min(resolution[1] - patch_radius_, center_pixel[1] + search_radius_+1)};
  return RenderingTile{begin, end};
}

/*!
  */
RenderingTile BayesianCollaborativeDenoiser::makeChunkTile(
    const Index2d& resolution,
    const Index2d& chunk_position,
    const Index2d& tile_position) const noexcept
{
  const uint chunk_size = getChunkSize();

  Index2d begin{patch_radius_ + chunk_size * chunk_position[0] +
                                search_radius_ * tile_position[0],
                patch_radius_ + chunk_size * chunk_position[1] +
                                search_radius_ * tile_position[1]};
  Index2d end{begin[0] + search_radius_, begin[1] + search_radius_};

  for (uint i = 0; i < 2; ++i) {
    begin[i] = zisc::min(begin[i], resolution[i] - patch_radius_);
    end[i] = zisc::min(end[i], resolution[i] - patch_radius_);
  }

  return RenderingTile{begin, end};
}

/*!
  */
template <uint kDimension>
void BayesianCollaborativeDenoiser::merge(
    System& system,
    Parameters<kDimension>* low_res_p,
    Parameters<kDimension>* high_res_p,
    zisc::pmr::vector<SpectraArray<kDimension>>* staging_value_table) const noexcept
{
  auto& threads = system.threadManager();
  auto& work_resource = system.globalMemoryManager();
  constexpr uint start = 0;
  const uint end = threads.numOfThreads();

  // Copy a high resolution buffer to a staging buffer
  for (uint i = 0; i < high_res_p->denoised_value_table_.size(); ++i)
    (*staging_value_table)[i] = -(high_res_p->denoised_value_table_[i]);

  auto merge1 = [&system, low_res_p, high_res_p, staging_value_table]
  (const int thread_id, const uint)
  {
    // Set the calculation range
    auto range = system.calcThreadRange(
        low_res_p->resolution_[0] * low_res_p->resolution_[1], thread_id);
    Parameters<kDimension>::template downscaleAverage<kDimension>(
        high_res_p->resolution_, *staging_value_table,
        low_res_p->resolution_, &low_res_p->sample_value_table_,
        Index2d{range[0], range[1]});

    // Set the calculation range
    range = system.calcThreadRange(
        high_res_p->resolution_[0] * high_res_p->resolution_[1], thread_id);
    Parameters<kDimension>::template upscaleAdd<kDimension>(
        low_res_p->resolution_, low_res_p->denoised_value_table_,
        high_res_p->resolution_, &high_res_p->denoised_value_table_,
        Index2d{range[0], range[1]});
  };

  {
//    auto result = threads.enqueueLoop(init_parameters, start, end, &work_resource);
//    result.wait();
#pragma omp parallel for
    for (uint thread_id = start; thread_id < end; ++thread_id)
      merge1(zisc::cast<int>(thread_id), 0);
  }

  auto merge2 = [&system, low_res_p, high_res_p]
  (const int thread_id, const uint)
  {
    // Set the calculation range
    const auto range = system.calcThreadRange(
        high_res_p->resolution_[0] * high_res_p->resolution_[1], thread_id);
    Parameters<kDimension>::template upscaleAdd<kDimension>(
        low_res_p->resolution_, low_res_p->sample_value_table_,
        high_res_p->resolution_, &high_res_p->denoised_value_table_,
        Index2d{range[0], range[1]});
  };

  {
//    auto result = threads.enqueueLoop(init_parameters, start, end, &work_resource);
//    result.wait();
#pragma omp parallel for
    for (uint thread_id = start; thread_id < end; ++thread_id)
      merge2(zisc::cast<int>(thread_id), 0);
  }
}

/*!
  */
template <uint kDimension>
auto BayesianCollaborativeDenoiser::selectSimilarPatches(
    const Parameters<kDimension>& parameters,
    const Index2d& main_pixel) const noexcept -> SimilarPatchMask
{
  SimilarPatchMask similar_mask;
  auto search_window = makeSearchWindow(parameters.resolution_, main_pixel);
  ZISC_ASSERT(search_window.numOfPixels() < similar_mask.size(),
              "The search window size is greater than the mask size.");
  for (uint p = 0; p < search_window.numOfPixels(); ++p) {
    const auto& neighbor_pixel = search_window.current();
    const Float d = (neighbor_pixel.data() != main_pixel.data())
        ? calcHistogramPatchDistance(parameters, main_pixel, neighbor_pixel)
        : 0.0;
    if (d <= histogram_distance_threshold_) {
      const uint index = search_window.getIndex(neighbor_pixel);
      similar_mask.set(index);
    }
    search_window.next();
  }
  return similar_mask;
}
/*!
  */
template <uint kDimension>
auto BayesianCollaborativeDenoiser::toMatrix(
    const CovarianceFactors<kDimension>& factors) const noexcept
        -> Matrix<kDimension>
{
  Matrix<kDimension> matrix;
  for (uint offset = 0, si_a = 0; si_a < kDimension; ++si_a) {
    for (uint si_b = si_a; si_b < kDimension; ++si_b, ++offset) {
      matrix(si_a, si_b) = factors[offset];
      matrix(si_b, si_a) = factors[offset];
    }
  }
  return matrix;
}

} // namespace nanairo