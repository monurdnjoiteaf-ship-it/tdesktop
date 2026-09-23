/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/audio/media_audio_edit.h"

#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/samplefmt.h>
} // extern "C"

namespace Media {
namespace {

using namespace FFmpeg;

constexpr auto kVoiceFrequency = 48'000;
constexpr auto kVoiceBitrate = 32'000;

struct AudioFifoDeleter {
	void operator()(AVAudioFifo *value) {
		av_audio_fifo_free(value);
	}
};
using AudioFifoPointer = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

[[nodiscard]] bool EncodeAndWrite(
		AVCodecContext *encoder,
		AVStream *stream,
		AVFormatContext *format,
		AVFrame *frame) {
	auto error = AvErrorWrap(avcodec_send_frame(encoder, frame));
	if (error) {
		LogError(u"avcodec_send_frame"_q, error);
		return false;
	}
	auto packet = av_packet_alloc();
	const auto guard = gsl::finally([&] {
		av_packet_free(&packet);
	});
	while (true) {
		error = AvErrorWrap(avcodec_receive_packet(encoder, packet));
		if (error.code() == AVERROR(EAGAIN)
			|| error.code() == AVERROR_EOF) {
			return true;
		} else if (error) {
			LogError(u"avcodec_receive_packet"_q, error);
			return false;
		}
		packet->stream_index = stream->index;
		av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
		error = AvErrorWrap(av_interleaved_write_frame(format, packet));
		if (error) {
			LogError(u"av_interleaved_write_frame"_q, error);
			return false;
		}
	}
}

class VoiceTranscoder final {
public:
	[[nodiscard]] bool init(
		not_null<AVFormatContext*> output,
		not_null<AVStream*> input);
	[[nodiscard]] bool process(
		not_null<AVFormatContext*> output,
		AVPacket *packet);
	[[nodiscard]] bool finish(not_null<AVFormatContext*> output);
	[[nodiscard]] crl::time duration() const;

private:
	[[nodiscard]] bool drainDecoder(not_null<AVFormatContext*> output);
	[[nodiscard]] bool pushToFifo(AVFrame *frame);
	[[nodiscard]] bool encodeFromFifo(
		not_null<AVFormatContext*> output,
		bool flushing);

	CodecPointer _decoder;
	CodecPointer _encoder;
	SwresamplePointer _swr;
	AudioFifoPointer _fifo;
	FramePointer _decodedFrame;
	FramePointer _encodeFrame;
	AVStream *_stream = nullptr;
	int64 _pts = 0;

};

bool VoiceTranscoder::init(
		not_null<AVFormatContext*> output,
		not_null<AVStream*> input) {
	_decoder = MakeCodecPointer({ .stream = input });
	if (!_decoder) {
		return false;
	}
	const auto codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		LogError(u"avcodec_find_encoder"_q, u"Opus"_q);
		return false;
	}
	_stream = avformat_new_stream(output, codec);
	if (!_stream) {
		return false;
	}
	_encoder = CodecPointer(avcodec_alloc_context3(codec));
	if (!_encoder) {
		return false;
	}
	av_channel_layout_default(&_encoder->ch_layout, 1);
	_encoder->codec_type = AVMEDIA_TYPE_AUDIO;
	_encoder->sample_fmt = codec->sample_fmts
		? codec->sample_fmts[0]
		: AV_SAMPLE_FMT_FLT;
	_encoder->sample_rate = kVoiceFrequency;
	_encoder->time_base = AVRational{ 1, kVoiceFrequency };
	_encoder->bit_rate = kVoiceBitrate;
	if (output->oformat->flags & AVFMT_GLOBALHEADER) {
		_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	auto error = AvErrorWrap(avcodec_open2(_encoder.get(), codec, nullptr));
	if (error) {
		LogError(u"avcodec_open2"_q, error, u"Opus"_q);
		return false;
	}
	error = AvErrorWrap(avcodec_parameters_from_context(
		_stream->codecpar,
		_encoder.get()));
	if (error) {
		return false;
	}
	_stream->time_base = _encoder->time_base;
	_fifo = AudioFifoPointer(av_audio_fifo_alloc(
		_encoder->sample_fmt,
		1,
		_encoder->frame_size * 4));
	_decodedFrame = MakeFramePointer();
	_encodeFrame = MakeFramePointer();
	return _fifo && _decodedFrame && _encodeFrame;
}

bool VoiceTranscoder::process(
		not_null<AVFormatContext*> output,
		AVPacket *packet) {
	const auto error = AvErrorWrap(avcodec_send_packet(
		_decoder.get(),
		packet));
	if (error) {
		LogError(u"avcodec_send_packet"_q, error);
		return (error.code() == AVERROR_INVALIDDATA);
	}
	return drainDecoder(output);
}

bool VoiceTranscoder::drainDecoder(not_null<AVFormatContext*> output) {
	while (true) {
		const auto error = AvErrorWrap(avcodec_receive_frame(
			_decoder.get(),
			_decodedFrame.get()));
		if (error.code() == AVERROR(EAGAIN)
			|| error.code() == AVERROR_EOF) {
			return true;
		} else if (error) {
			LogError(u"avcodec_receive_frame"_q, error);
			return (error.code() == AVERROR_INVALIDDATA);
		}
		_swr = MakeSwresamplePointer(
			&_decodedFrame->ch_layout,
			AVSampleFormat(_decodedFrame->format),
			_decodedFrame->sample_rate,
			&_encoder->ch_layout,
			_encoder->sample_fmt,
			_encoder->sample_rate,
			&_swr);
		if (!_swr
			|| !pushToFifo(_decodedFrame.get())
			|| !encodeFromFifo(output, false)) {
			return false;
		}
	}
}

bool VoiceTranscoder::pushToFifo(AVFrame *frame) {
	const auto inputSamples = frame ? frame->nb_samples : 0;
	const auto maximum = int(swr_get_out_samples(_swr.get(), inputSamples));
	if (maximum <= 0) {
		return true;
	}
	auto converted = MakeFramePointer();
	converted->nb_samples = maximum;
	converted->format = _encoder->sample_fmt;
	converted->sample_rate = _encoder->sample_rate;
	av_channel_layout_copy(&converted->ch_layout, &_encoder->ch_layout);
	const auto error = AvErrorWrap(av_frame_get_buffer(converted.get(), 0));
	if (error) {
		return false;
	}
	const auto samples = swr_convert(
		_swr.get(),
		converted->extended_data,
		maximum,
		frame
			? const_cast<const uint8_t**>(frame->extended_data)
			: nullptr,
		inputSamples);
	if (samples < 0) {
		return false;
	} else if (!samples) {
		return true;
	}
	return av_audio_fifo_write(
		_fifo.get(),
		reinterpret_cast<void**>(converted->extended_data),
		samples) == samples;
}

bool VoiceTranscoder::encodeFromFifo(
		not_null<AVFormatContext*> output,
		bool flushing) {
	const auto frameSize = _encoder->frame_size;
	while (true) {
		const auto available = av_audio_fifo_size(_fifo.get());
		if (available <= 0 || (!flushing && available < frameSize)) {
			return true;
		}
		const auto take = std::min(available, frameSize);
		av_frame_unref(_encodeFrame.get());
		_encodeFrame->nb_samples = frameSize;
		_encodeFrame->format = _encoder->sample_fmt;
		_encodeFrame->sample_rate = _encoder->sample_rate;
		av_channel_layout_copy(
			&_encodeFrame->ch_layout,
			&_encoder->ch_layout);
		if (AvErrorWrap(av_frame_get_buffer(_encodeFrame.get(), 0))) {
			return false;
		}
		const auto read = av_audio_fifo_read(
			_fifo.get(),
			reinterpret_cast<void**>(_encodeFrame->extended_data),
			take);
		if (read < take) {
			return false;
		}
		if (take < frameSize) {
			av_samples_set_silence(
				_encodeFrame->extended_data,
				take,
				frameSize - take,
				1,
				_encoder->sample_fmt);
		}
		_encodeFrame->pts = _pts;
		_pts += take;
		if (!EncodeAndWrite(
				_encoder.get(),
				_stream,
				output,
				_encodeFrame.get())) {
			return false;
		}
	}
}

bool VoiceTranscoder::finish(not_null<AVFormatContext*> output) {
	const auto sent = AvErrorWrap(avcodec_send_packet(_decoder.get(), nullptr));
	if (!sent && !drainDecoder(output)) {
		return false;
	}
	return (!_swr || pushToFifo(nullptr))
		&& encodeFromFifo(output, true)
		&& EncodeAndWrite(_encoder.get(), _stream, output, nullptr);
}

crl::time VoiceTranscoder::duration() const {
	return (_pts * crl::time(1000)) / kVoiceFrequency;
}

} // namespace

[[nodiscard]] AudioEditResult TrimAudioToRange(
		const QByteArray &content,
		crl::time from,
		crl::time till) {
	using namespace FFmpeg;

	if (content.isEmpty() || (from < 0) || (till <= from)) {
		return {};
	}

	auto inputWrap = ReadBytesWrap{
		.size = content.size(),
		.data = reinterpret_cast<const uchar*>(content.constData()),
	};
	auto input = MakeFormatPointer(
		&inputWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
	if (!input) {
		return {};
	}

	auto error = AvErrorWrap(avformat_find_stream_info(input.get(), nullptr));
	if (error) {
		LogError(u"avformat_find_stream_info"_q, error);
		return {};
	}

	const auto streamId = av_find_best_stream(
		input.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (streamId < 0) {
		LogError(u"av_find_best_stream"_q, AvErrorWrap(streamId));
		return {};
	}

	const auto inStream = input->streams[streamId];
	auto outputWrap = WriteBytesWrap();
	auto output = MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"opus"_q);
	if (!output) {
		return {};
	}

	const auto outStream = avformat_new_stream(output.get(), nullptr);
	if (!outStream) {
		LogError(u"avformat_new_stream"_q);
		return {};
	}

	error = AvErrorWrap(avcodec_parameters_copy(
		outStream->codecpar,
		inStream->codecpar));
	if (error) {
		LogError(u"avcodec_parameters_copy"_q, error);
		return {};
	}
	outStream->codecpar->codec_tag = 0;
	outStream->time_base = inStream->time_base;

	error = AvErrorWrap(avformat_write_header(output.get(), nullptr));
	if (error) {
		LogError(u"avformat_write_header"_q, error);
		return {};
	}

	const auto fromPts = TimeToPts(from, inStream->time_base);
	const auto tillPts = TimeToPts(till, inStream->time_base);
	auto firstPts = int64(AV_NOPTS_VALUE);
	auto firstDts = int64(AV_NOPTS_VALUE);
	auto lastPts = std::numeric_limits<int64>::min();
	auto lastDts = std::numeric_limits<int64>::min();
	auto durationPts = int64(0);
	auto copied = 0;

	auto packet = AVPacket();
	av_init_packet(&packet);
	while (true) {
		error = AvErrorWrap(av_read_frame(input.get(), &packet));
		if (error.code() == AVERROR_EOF) {
			break;
		} else if (error) {
			LogError(u"av_read_frame"_q, error);
			return {};
		}
		const auto guard = gsl::finally([&] {
			av_packet_unref(&packet);
		});
		if (packet.stream_index != streamId) {
			continue;
		}

		const auto packetStart = (packet.pts != AV_NOPTS_VALUE)
			? packet.pts
			: packet.dts;
		const auto packetDuration = std::max(int64(packet.duration), int64());
		const auto packetEnd = (packetStart != AV_NOPTS_VALUE)
			? (packetStart + packetDuration)
			: AV_NOPTS_VALUE;
		if ((packetStart != AV_NOPTS_VALUE) && (packetStart >= tillPts)) {
			break;
		}
		if ((packetEnd != AV_NOPTS_VALUE) && (packetEnd <= fromPts)) {
			continue;
		}

		if (packet.pts != AV_NOPTS_VALUE) {
			if (firstPts == AV_NOPTS_VALUE) {
				firstPts = packet.pts;
			}
			packet.pts -= firstPts;
			if (packet.pts < 0) {
				packet.pts = 0;
			}
			if (packet.pts <= lastPts) {
				packet.pts = lastPts + 1;
			}
			lastPts = packet.pts;
		}
		if (packet.dts != AV_NOPTS_VALUE) {
			if (firstDts == AV_NOPTS_VALUE) {
				firstDts = packet.dts;
			}
			packet.dts -= firstDts;
			if (packet.dts < 0) {
				packet.dts = 0;
			}
			if (packet.dts <= lastDts) {
				packet.dts = lastDts + 1;
			}
			lastDts = packet.dts;
		}

		const auto packetPosition = (packet.pts != AV_NOPTS_VALUE)
			? packet.pts
			: packet.dts;
		if (packetPosition != AV_NOPTS_VALUE) {
			durationPts = std::max(
				durationPts,
				packetPosition + std::max(int64(packet.duration), int64()));
		}

		packet.stream_index = outStream->index;
		error = AvErrorWrap(av_interleaved_write_frame(output.get(), &packet));
		if (error) {
			LogError(u"av_interleaved_write_frame"_q, error);
			return {};
		}
		++copied;
	}

	if (!copied) {
		return {};
	}

	error = AvErrorWrap(av_write_trailer(output.get()));
	if (error) {
		LogError(u"av_write_trailer"_q, error);
		return {};
	}

	auto result = AudioEditResult();
	result.content = std::move(outputWrap.content);
	result.waveform = audioCountWaveform(Core::FileLocation(), result.content);
	result.duration = durationPts
		? PtsToTimeCeil(durationPts, outStream->time_base)
		: (till - from);
	return result;
}

[[nodiscard]] AudioEditResult ConcatAudio(
		const QByteArray &first,
		const QByteArray &second) {
	using namespace FFmpeg;

	if (first.isEmpty() || second.isEmpty()) {
		return {};
	}

	auto firstWrap = ReadBytesWrap{
		.size = first.size(),
		.data = reinterpret_cast<const uchar*>(first.constData()),
	};
	auto firstInput = MakeFormatPointer(
		&firstWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
	if (!firstInput) {
		return {};
	}

	auto secondWrap = ReadBytesWrap{
		.size = second.size(),
		.data = reinterpret_cast<const uchar*>(second.constData()),
	};
	auto secondInput = MakeFormatPointer(
		&secondWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
	if (!secondInput) {
		return {};
	}

	const auto prepareStream = [](not_null<AVFormatContext*> input) {
		auto error = AvErrorWrap(avformat_find_stream_info(input, nullptr));
		if (error) {
			LogError(u"avformat_find_stream_info"_q, error);
			return static_cast<AVStream*>(nullptr);
		}
		const auto streamId = av_find_best_stream(
			input,
			AVMEDIA_TYPE_AUDIO,
			-1,
			-1,
			nullptr,
			0);
		if (streamId < 0) {
			LogError(u"av_find_best_stream"_q, AvErrorWrap(streamId));
			return static_cast<AVStream*>(nullptr);
		}
		return input->streams[streamId];
	};
	const auto firstStream = prepareStream(firstInput.get());
	if (!firstStream) {
		return {};
	}
	const auto secondStream = prepareStream(secondInput.get());
	if (!secondStream) {
		return {};
	}

	auto outputWrap = WriteBytesWrap();
	auto output = MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"opus"_q);
	if (!output) {
		return {};
	}
	const auto outStream = avformat_new_stream(output.get(), nullptr);
	if (!outStream) {
		LogError(u"avformat_new_stream"_q);
		return {};
	}

	auto error = AvErrorWrap(avcodec_parameters_copy(
		outStream->codecpar,
		firstStream->codecpar));
	if (error) {
		LogError(u"avcodec_parameters_copy"_q, error);
		return {};
	}
	outStream->codecpar->codec_tag = 0;
	outStream->time_base = firstStream->time_base;

	error = AvErrorWrap(avformat_write_header(output.get(), nullptr));
	if (error) {
		LogError(u"avformat_write_header"_q, error);
		return {};
	}

	auto offsetPts = int64(0);
	auto durationPts = int64(0);
	auto lastPts = std::numeric_limits<int64>::min();
	auto lastDts = std::numeric_limits<int64>::min();
	auto copied = 0;

	const auto append = [&](
			not_null<AVFormatContext*> input,
			not_null<AVStream*> inStream) {
		auto firstPts = int64(AV_NOPTS_VALUE);
		auto firstDts = int64(AV_NOPTS_VALUE);
		auto sourceEndPts = offsetPts;

		auto packet = AVPacket();
		av_init_packet(&packet);
		while (true) {
			auto error = AvErrorWrap(av_read_frame(input, &packet));
			if (error.code() == AVERROR_EOF) {
				break;
			} else if (error) {
				LogError(u"av_read_frame"_q, error);
				return false;
			}
			const auto guard = gsl::finally([&] {
				av_packet_unref(&packet);
			});
			if (packet.stream_index != inStream->index) {
				continue;
			}

			if (packet.pts != AV_NOPTS_VALUE) {
				if (firstPts == AV_NOPTS_VALUE) {
					firstPts = packet.pts;
				}
				packet.pts -= firstPts;
				if (packet.pts < 0) {
					packet.pts = 0;
				}
				packet.pts = av_rescale_q(
					packet.pts,
					inStream->time_base,
					outStream->time_base);
				packet.pts += offsetPts;
				if (packet.pts <= lastPts) {
					packet.pts = lastPts + 1;
				}
				lastPts = packet.pts;
			}
			if (packet.dts != AV_NOPTS_VALUE) {
				if (firstDts == AV_NOPTS_VALUE) {
					firstDts = packet.dts;
				}
				packet.dts -= firstDts;
				if (packet.dts < 0) {
					packet.dts = 0;
				}
				packet.dts = av_rescale_q(
					packet.dts,
					inStream->time_base,
					outStream->time_base);
				packet.dts += offsetPts;
				if (packet.dts <= lastDts) {
					packet.dts = lastDts + 1;
				}
				lastDts = packet.dts;
			}
			packet.duration = (packet.duration > 0)
				? av_rescale_q(
					packet.duration,
					inStream->time_base,
					outStream->time_base)
				: 0;

			const auto packetPosition = (packet.pts != AV_NOPTS_VALUE)
				? packet.pts
				: packet.dts;
			if (packetPosition != AV_NOPTS_VALUE) {
				const auto packetEnd = packetPosition
					+ std::max(int64(packet.duration), int64());
				durationPts = std::max(durationPts, packetEnd);
				sourceEndPts = std::max(sourceEndPts, packetEnd);
			}

			packet.stream_index = outStream->index;
			error = AvErrorWrap(av_interleaved_write_frame(output.get(), &packet));
			if (error) {
				LogError(u"av_interleaved_write_frame"_q, error);
				return false;
			}
			++copied;
		}

		offsetPts = sourceEndPts;
		return true;
	};
	if (!append(firstInput.get(), firstStream)
		|| !append(secondInput.get(), secondStream)) {
		return {};
	}
	if (!copied) {
		return {};
	}

	error = AvErrorWrap(av_write_trailer(output.get()));
	if (error) {
		LogError(u"av_write_trailer"_q, error);
		return {};
	}

	auto result = AudioEditResult();
	result.content = std::move(outputWrap.content);
	result.waveform = audioCountWaveform(Core::FileLocation(), result.content);
	result.duration = durationPts
		? PtsToTimeCeil(durationPts, outStream->time_base)
		: 0;
	return result;
}

AudioEditResult ConvertAudioToVoice(const QByteArray &content) {
	if (content.isEmpty()) {
		return {};
	}
	auto inputWrap = ReadBytesWrap{
		.size = content.size(),
		.data = reinterpret_cast<const uchar*>(content.constData()),
	};
	auto input = MakeFormatPointer(
		&inputWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
	if (!input
		|| AvErrorWrap(avformat_find_stream_info(input.get(), nullptr))) {
		return {};
	}
	const auto streamId = av_find_best_stream(
		input.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (streamId < 0) {
		return {};
	}
	auto outputWrap = WriteBytesWrap();
	auto output = MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"opus"_q);
	if (!output) {
		return {};
	}
	auto transcoder = VoiceTranscoder();
	if (!transcoder.init(output.get(), input->streams[streamId])
		|| AvErrorWrap(avformat_write_header(output.get(), nullptr))) {
		return {};
	}
	auto packet = AVPacket();
	av_init_packet(&packet);
	while (true) {
		const auto error = AvErrorWrap(av_read_frame(input.get(), &packet));
		if (error.code() == AVERROR_EOF) {
			break;
		} else if (error) {
			return {};
		}
		const auto guard = gsl::finally([&] {
			av_packet_unref(&packet);
		});
		if (packet.stream_index == streamId
			&& !transcoder.process(output.get(), &packet)) {
			return {};
		}
	}
	if (!transcoder.finish(output.get())
		|| AvErrorWrap(av_write_trailer(output.get()))) {
		return {};
	}
	auto result = AudioEditResult();
	result.content = std::move(outputWrap.content);
	result.waveform = audioCountWaveform(Core::FileLocation(), result.content);
	result.duration = transcoder.duration();
	return result;
}

} // namespace Media
