// Copyright (c) 2021 Michael Fabian Dirks <info@xaymar.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "nvidia-vfx-greenscreen.hpp"
#include <cmath>
#include <utility>
#include "nvidia/cv/nvidia-cv.hpp"
#include "obs/gs/gs-helper.hpp"
#include "util/util-logging.hpp"
#include "util/utility.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<nvidia::vfx::greenscreen::greenscreen> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

// TODO: Figure out actual latency, appears to be either 2 or 3 frames.
#define LATENCY_BUFFER 2

streamfx::nvidia::vfx::greenscreen::~greenscreen()
{
	// Enter Contexts.
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	_tmp.reset();
	_output.reset();
	_destination.reset();
	_source.reset();
	_input.reset();
	_buffer.clear();
}

streamfx::nvidia::vfx::greenscreen::greenscreen()
	: effect(EFFECT_GREEN_SCREEN), _dirty(true), _input(), _source(), _destination(), _output(), _tmp()
{
	// Enter Contexts.
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	// Mode
	set_mode(greenscreen_mode::QUALITY);

	// Allocate resources
	resize(512, 288);
}

void streamfx::nvidia::vfx::greenscreen::size(std::pair<uint32_t, uint32_t>& size)
{
	constexpr uint32_t min_width  = 512;
	constexpr uint32_t min_height = 288;

	// Calculate Size
	if (size.first > size.second) {
		// Dominant Width
		double ar  = static_cast<double>(size.second) / static_cast<double>(size.first);
		size.first = std::max<uint32_t>(size.first, min_width);
		size.second =
			std::max<uint32_t>(static_cast<uint32_t>(std::lround(static_cast<double>(size.first) * ar)), min_height);
	} else {
		// Dominant Height
		double ar   = static_cast<double>(size.first) / static_cast<double>(size.second);
		size.second = std::max<uint32_t>(size.second, min_height);
		size.first =
			std::max<uint32_t>(static_cast<uint32_t>(std::lround(static_cast<double>(size.second) * ar)), min_width);
	}
}

void streamfx::nvidia::vfx::greenscreen::set_mode(greenscreen_mode mode)
{
	set(PARAMETER_MODE, static_cast<uint32_t>(mode));
	_dirty = true;
}

std::shared_ptr<streamfx::obs::gs::texture>
	streamfx::nvidia::vfx::greenscreen::process(std::shared_ptr<::streamfx::obs::gs::texture> in)
{
	// Enter Graphics and CUDA context.
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = _nvcuda->get_context()->enter();

#ifdef ENABLE_PROFILING
	::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_magenta, "NvVFX Background Removal"};
#endif

	// Resize if the size or scale was changed.
	resize(in->get_width(), in->get_height());

	// Reload effect if dirty.
	if (_dirty) {
		load();
	}

	{ // Copy parameter to input.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy, "Copy In -> Input"};
#endif
		gs_copy_texture(_input->get_texture()->get_object(), in->get_object());
	}

	{ // Enqueue into buffer (back is newest).
		auto el = _buffer.front();
		gs_copy_texture(el->get_object(), in->get_object());
		_buffer.push_back(el);
		_buffer.pop_front();
	}

	{ // Copy input to source.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy, "Copy Input -> Source"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_input->get_image(), _source->get_image(), 1.f,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer input to processing source due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	{ // Process source to destination.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_cache, "Process"};
#endif
		if (auto res = run(); res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to process due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Run failed.");
		}
	}

	{ // Copy destination to output.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy,
													"Copy Destination -> Output"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_destination->get_image(), _output->get_image(), 1.,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer processing result to output due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	// Return output.
	return _output->get_texture();
}

std::shared_ptr<streamfx::obs::gs::texture> streamfx::nvidia::vfx::greenscreen::get_color()
{
	//return _input->get_texture();
	return _buffer.front();
}

std::shared_ptr<streamfx::obs::gs::texture> streamfx::nvidia::vfx::greenscreen::get_mask()
{
	return _output->get_texture();
}

void streamfx::nvidia::vfx::greenscreen::resize(uint32_t width, uint32_t height)
{
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	std::pair<uint32_t, uint32_t> in_size = {width, height};
	size(in_size);

	if (!_tmp) {
		_tmp = std::make_shared<::streamfx::nvidia::cv::image>(
			width, height, ::streamfx::nvidia::cv::pixel_format::RGBA, ::streamfx::nvidia::cv::component_type::UINT8,
			::streamfx::nvidia::cv::component_layout::PLANAR, ::streamfx::nvidia::cv::memory_location::GPU, 1);
	}

	if (!_input || (in_size.first != _input->get_texture()->get_width())
		|| (in_size.second != _input->get_texture()->get_height())) {
		{
			_buffer.clear();
			for (size_t idx = 0; idx < LATENCY_BUFFER; idx++) {
				auto el = std::make_shared<::streamfx::obs::gs::texture>(width, height, GS_RGBA_UNORM, 1, nullptr,
																		 ::streamfx::obs::gs::texture::flags::None);
				_buffer.push_back(el);
			}
		}

		if (_input) {
			_input->resize(in_size.first, in_size.second);
		} else {
			_input = std::make_shared<::streamfx::nvidia::cv::texture>(in_size.first, in_size.second, GS_RGBA_UNORM);
		}

		_dirty = true;
	}

	if (!_source || (in_size.first != _source->get_image()->width)
		|| (in_size.second != _source->get_image()->height)) {
		if (_source) {
			_source->resize(in_size.first, in_size.second);
		} else {
			_source = std::make_shared<::streamfx::nvidia::cv::image>(
				in_size.first, in_size.second, ::streamfx::nvidia::cv::pixel_format::BGR,
				::streamfx::nvidia::cv::component_type::UINT8, ::streamfx::nvidia::cv::component_layout::INTERLEAVED,
				::streamfx::nvidia::cv::memory_location::GPU, 1);
		}

		if (auto v = set(PARAMETER_INPUT_IMAGE_0, _source); v != ::streamfx::nvidia::cv::result::SUCCESS) {
			throw ::streamfx::nvidia::cv::exception(PARAMETER_INPUT_IMAGE_0, v);
		}

		_dirty = true;
	}

	if (!_destination || (in_size.first != _destination->get_image()->width)
		|| (in_size.second != _destination->get_image()->height)) {
		if (_destination) {
			_destination->resize(in_size.first, in_size.second);
		} else {
			_destination = std::make_shared<::streamfx::nvidia::cv::image>(
				in_size.first, in_size.second, ::streamfx::nvidia::cv::pixel_format::A,
				::streamfx::nvidia::cv::component_type::UINT8, ::streamfx::nvidia::cv::component_layout::INTERLEAVED,
				::streamfx::nvidia::cv::memory_location::GPU, 1);
		}

		if (auto v = set(PARAMETER_OUTPUT_IMAGE_0, _destination); v != ::streamfx::nvidia::cv::result::SUCCESS) {
			throw ::streamfx::nvidia::cv::exception(PARAMETER_OUTPUT_IMAGE_0, v);
		}

		_dirty = true;
	}

	if (!_output || (in_size.first != _output->get_texture()->get_width())
		|| (in_size.second != _output->get_texture()->get_height())) {
		if (_output) {
			_output->resize(in_size.first, in_size.second);
		} else {
			_output = std::make_shared<::streamfx::nvidia::cv::texture>(in_size.first, in_size.second, GS_A8);
		}

		_dirty = true;
	}
}

void streamfx::nvidia::vfx::greenscreen::load()
{
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = _nvcuda->get_context()->enter();

	// Assign CUDA Stream object.
	if (auto v = set(PARAMETER_CUDA_STREAM, _nvcuda->get_stream()); v != cv::result::SUCCESS) {
		throw ::streamfx::nvidia::cv::exception(PARAMETER_CUDA_STREAM, v);
	}

	if (auto v = effect::load(); v != ::streamfx::nvidia::cv::result::SUCCESS) {
		throw ::streamfx::nvidia::cv::exception("load", v);
	}

	_dirty = false;
}
