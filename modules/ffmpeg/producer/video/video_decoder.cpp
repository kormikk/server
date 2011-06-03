/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/
#include "../../stdafx.h"

#include "video_decoder.h"
#include "../../ffmpeg_error.h"

#include <common/memory/memcpy.h>

#include <core/video_format.h>
#include <core/producer/frame/basic_frame.h>
#include <core/mixer/write_frame.h>
#include <core/producer/frame/image_transform.h>
#include <core/producer/frame/pixel_format.h>
#include <core/producer/frame/frame_factory.h>

#include <tbb/parallel_for.h>

#include <boost/range/algorithm_ext.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libswscale/swscale.h>
	#include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar {
	
core::pixel_format::type get_pixel_format(PixelFormat pix_fmt)
{
	switch(pix_fmt)
	{
	case PIX_FMT_GRAY8:		return core::pixel_format::gray;
	case PIX_FMT_BGRA:		return core::pixel_format::bgra;
	case PIX_FMT_ARGB:		return core::pixel_format::argb;
	case PIX_FMT_RGBA:		return core::pixel_format::rgba;
	case PIX_FMT_ABGR:		return core::pixel_format::abgr;
	case PIX_FMT_YUV444P:	return core::pixel_format::ycbcr;
	case PIX_FMT_YUV422P:	return core::pixel_format::ycbcr;
	case PIX_FMT_YUV420P:	return core::pixel_format::ycbcr;
	case PIX_FMT_YUV411P:	return core::pixel_format::ycbcr;
	case PIX_FMT_YUV410P:	return core::pixel_format::ycbcr;
	case PIX_FMT_YUVA420P:	return core::pixel_format::ycbcra;
	default:				return core::pixel_format::invalid;
	}
}

core::pixel_format_desc get_pixel_format_desc(PixelFormat pix_fmt, size_t width, size_t height)
{
	// Get linesizes
	AVPicture dummy_pict;	
	avpicture_fill(&dummy_pict, nullptr, pix_fmt, width, height);

	core::pixel_format_desc desc;
	desc.pix_fmt = get_pixel_format(pix_fmt);
		
	switch(desc.pix_fmt)
	{
	case core::pixel_format::gray:
		{
			desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[0]/4, height, 1));						
			return desc;
		}
	case core::pixel_format::bgra:
	case core::pixel_format::argb:
	case core::pixel_format::rgba:
	case core::pixel_format::abgr:
		{
			desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[0]/4, height, 4));						
			return desc;
		}
	case core::pixel_format::ycbcr:
	case core::pixel_format::ycbcra:
		{		
			// Find chroma height
			size_t size2 = dummy_pict.data[2] - dummy_pict.data[1];
			size_t h2 = size2/dummy_pict.linesize[1];			

			desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[0], height, 1));
			desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[1], h2, 1));
			desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[2], h2, 1));

			if(desc.pix_fmt == core::pixel_format::ycbcra)						
				desc.planes.push_back(core::pixel_format_desc::plane(dummy_pict.linesize[3], height, 1));	
			return desc;
		}		
	default:		
		desc.pix_fmt = core::pixel_format::invalid;
		return desc;
	}
}

struct video_decoder::implementation : boost::noncopyable
{
	input& input_;
	std::shared_ptr<SwsContext>					sws_context_;
	const std::shared_ptr<core::frame_factory>	frame_factory_;
	AVCodecContext&								codec_context_;
	const int									width_;
	const int									height_;
	const PixelFormat							pix_fmt_;
	core::pixel_format_desc						desc_;
	size_t										frame_number_;

public:
	explicit implementation(input& input, const safe_ptr<core::frame_factory>& frame_factory) 
		: input_(input)
		, frame_factory_(frame_factory)
		, codec_context_(*input_.get_video_codec_context())
		, width_(codec_context_.width)
		, height_(codec_context_.height)
		, pix_fmt_(codec_context_.pix_fmt)
		, desc_(get_pixel_format_desc(pix_fmt_, width_, height_))
		, frame_number_(0)
	{
		if(desc_.pix_fmt == core::pixel_format::invalid)
		{
			CASPAR_LOG(warning) << "Hardware accelerated color transform not supported.";

			desc_ = get_pixel_format_desc(PIX_FMT_BGRA, width_, height_);
			double param;
			sws_context_.reset(sws_getContext(width_, height_, pix_fmt_, width_, height_, PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, &param), sws_freeContext);
			if(!sws_context_)
				BOOST_THROW_EXCEPTION(operation_failed() <<
									  msg_info("Could not create software scaling context.") << 
									  boost::errinfo_api_function("sws_getContext"));
		}
	}

	std::deque<std::pair<int, safe_ptr<core::write_frame>>> receive()
	{
		std::deque<std::pair<int, safe_ptr<core::write_frame>>> result;
		
		std::shared_ptr<AVPacket> pkt;
		for(int n = 0; n < 32 && result.empty() && input_.try_pop_video_packet(pkt); ++n)	
			boost::range::push_back(result, decode(pkt));

		return result;
	}

	std::deque<std::pair<int, safe_ptr<core::write_frame>>> decode(const std::shared_ptr<AVPacket>& video_packet)
	{			
		std::deque<std::pair<int, safe_ptr<core::write_frame>>> result;

		if(!video_packet) // eof
		{	
			avcodec_flush_buffers(&codec_context_);
			frame_number_ = 0;
			return result;
		}

		safe_ptr<AVFrame> decoded_frame(avcodec_alloc_frame(), av_free);

		int frame_finished = 0;
		const int errn = avcodec_decode_video2(&codec_context_, decoded_frame.get(), &frame_finished, video_packet.get());
		
		if(errn < 0)
		{
			BOOST_THROW_EXCEPTION(
				invalid_operation() <<
				msg_info(av_error_str(errn)) <<
				boost::errinfo_api_function("avcodec_decode_video") <<
				boost::errinfo_errno(AVUNERROR(errn)));
		}
		
		if(frame_finished != 0)		
			result.push_back(std::make_pair(frame_number_++, make_write_frame(decoded_frame)));

		return result;
	}

	safe_ptr<core::write_frame> make_write_frame(safe_ptr<AVFrame> decoded_frame)
	{		
		auto write = frame_factory_->create_frame(this, desc_);
		if(sws_context_ == nullptr)
		{
			tbb::parallel_for(0, static_cast<int>(desc_.planes.size()), 1, [&](int n)
			{
				auto plane            = desc_.planes[n];
				auto result           = write->image_data(n).begin();
				auto decoded          = decoded_frame->data[n];
				auto decoded_linesize = decoded_frame->linesize[n];
				
				// Copy line by line since ffmpeg sometimes pads each line.
				tbb::parallel_for(tbb::blocked_range<size_t>(0, static_cast<int>(desc_.planes[n].height)), [&](const tbb::blocked_range<size_t>& r)
				{
					for(size_t y = r.begin(); y != r.end(); ++y)
						memcpy(result + y*plane.linesize, decoded + y*decoded_linesize, plane.linesize);
				});

				write->commit(n);
			});
		}
		else
		{
			// Use sws_scale when provided colorspace has no hw-accel.
			safe_ptr<AVFrame> av_frame(avcodec_alloc_frame(), av_free);	
			avcodec_get_frame_defaults(av_frame.get());			
			avpicture_fill(reinterpret_cast<AVPicture*>(av_frame.get()), write->image_data().begin(), PIX_FMT_BGRA, width_, height_);
		 
			sws_scale(sws_context_.get(), decoded_frame->data, decoded_frame->linesize, 0, height_, av_frame->data, av_frame->linesize);	

			write->commit();
		}	

		// DVVIDEO is in lower field. Make it upper field if needed.
		if(codec_context_.codec_id == CODEC_ID_DVVIDEO && frame_factory_->get_video_format_desc().mode == core::video_mode::upper)
			write->get_image_transform().set_fill_translation(0.0f, 0.5/static_cast<double>(height_));

		return write;
	}
};

video_decoder::video_decoder(input& input, const safe_ptr<core::frame_factory>& frame_factory) : impl_(new implementation(input, frame_factory)){}
std::deque<std::pair<int, safe_ptr<core::write_frame>>> video_decoder::receive(){return impl_->receive();}

}