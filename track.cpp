//==================================================================//
/*
	Untrunc - track.cpp

	Untrunc is GPL software; you can freely distribute,
	redistribute, modify & use under the terms of the GNU General
	Public License; either version 2 or its successor.

	Untrunc is distributed under the GPL "AS IS", without
	any warranty; without the implied warranty of merchantability
	or fitness for either an expressed or implied particular purpose.

	Please see the included GNU General Public License (GPL) for
	your rights and further details; see the file COPYING. If you
	cannot, write to the Free Software Foundation, 59 Temple Place
	Suite 330, Boston, MA 02111-1307, USA.  Or www.fsf.org

	Copyright 2010 Federico Ponchio
                                                                    */
//==================================================================//

#include <vector>

#include <iostream>
//#include <iomanip>
#include <cstring>
#include <cassert>

#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS    1
#endif
#ifndef __STDC_CONSTANT_MACROS
# define __STDC_CONSTANT_MACROS 1
#endif
extern "C" {
#include <stdint.h>
}
#ifndef INT64_C
# define INT64_C(c)     (c ## LL)
# define UINT64_C(c)    (c ## ULL)
#endif

// Prevent #define of bool, true & false.
#undef  __bool_true_false_are_defined
#define __bool_true_false_are_defined   1
#ifndef _Bool
# define _Bool      bool
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
//#include <libavutil/log.h>

// WARNING: Including internal headers!
#if (LIBAVCODEC_VERSION_MAJOR != 56)    // Ubuntu 16.04
# include <config.h>
#endif
// XXX Horrible Hack: Suppress C99 keywords that are not in C++, like 'restrict' and '_Atomic'! XXX
#undef  restrict    // Harmless; don't restrict memory access.
#define restrict
#undef  _Atomic     // Atomics are only included in headers, but never actually used in our code.
#define _Atomic
// XXX Horrible Hack: There are variables named 'new' and 'class' inside! XXX
#define new         extern_new
#define class       extern_class
#include <libavcodec/h264dec.h>
#undef class
#undef new
#undef _Atomic
#undef restrict
}

#include "track.h"
#include "atom.h"


using namespace std;


// Additional output.
#define VERBOSE         1


namespace {
	// Read an unaligned, big-endian value.
	// A compiler will optimize this (at -O2) to a single instruction if possible.
	template<class T>
	static inline T readBE(const uint8_t *p, size_t i = 0) {
		return (i >= sizeof(T)) ? T(0) :
				(T(*p) << ((sizeof(T) - 1 - i) * 8)) | readBE<T>(p + 1, i + 1);
	};

	template<class T>
	static inline void readBE(T &result, const uint8_t *p) { result = readBE<T>(p); };

	// Write an unaligned, big-endian value.
	template<class T>
	static inline void writeBE(uint8_t *p, T value, size_t i = 0) {
		(i >= sizeof(T)) ? void(0) :
			(*p = ((value >> ((sizeof(T) - 1 - i) * 8)) & 0xFF) , writeBE(p + 1, value, i + 1));
	};


	// Configure FFmpeg/Libav logging for use in C++.
	class AvLog {
		int lvl;
#ifdef AV_LOG_PRINT_LEVEL
		int flgs;
#endif
	public:
#ifdef AV_LOG_PRINT_LEVEL
# define DEFAULT_AVLOG_FLAGS	AV_LOG_PRINT_LEVEL
#else
# define DEFAULT_AVLOG_FLAGS	0
#endif

		explicit AvLog()
			: lvl(av_log_get_level())
#ifdef AV_LOG_PRINT_LEVEL
			, flgs(av_log_get_flags())
#endif
		{
			av_log_set_flags(DEFAULT_AVLOG_FLAGS);
			cout.flush();   // Flush C++ standard streams.
			cerr.flush();   // Default unbuffered -> Flush in case this has changed.
			clog.flush();
		}

		explicit AvLog(int level, int flags = DEFAULT_AVLOG_FLAGS)
			: lvl(av_log_get_level())
#ifdef AV_LOG_PRINT_LEVEL
			, flgs(av_log_get_flags())
#endif
		{
			if(lvl < level)
				av_log_set_level(lvl);
			av_log_set_flags(flags);
			cout.flush();   // Flush C++ standard streams.
			cerr.flush();   // Default unbuffered -> Flush in case this has changed.
			clog.flush();
		}

		~AvLog() {
			fflush(stdout); // Flush C stdio files.
			fflush(stderr);
			av_log_set_level(lvl);
#ifdef AV_LOG_PRINT_LEVEL
			av_log_set_flags(flgs);
#endif
		}
	};
}; // namespace



// AVC1
class H264sps {
public:
	int  log2_max_frame_num;
	bool frame_mbs_only_flag;
	int  poc_type;
	int  log2_max_poc_lsb;

	H264sps()
		: log2_max_frame_num(0),
		  frame_mbs_only_flag(false),
		  poc_type(0),
		  log2_max_poc_lsb(0)
	{ }

	H264sps(const SPS &avsps)
		: log2_max_frame_num(avsps.log2_max_frame_num),
		  frame_mbs_only_flag(bool(avsps.frame_mbs_only_flag)),
		  poc_type(avsps.poc_type),
		  log2_max_poc_lsb(avsps.log2_max_poc_lsb)
	{ }

	H264sps &operator=(const SPS &avsps) { return *this = H264sps(avsps); }

	void parseSPS(const uint8_t *data, int size);
};


void H264sps::parseSPS(const uint8_t *data, int size) {
	assert(data != NULL);
	if (data[0] != 1) {
		cerr << "Uncharted territory...\n";
	}

	if (size < 7) {
		throw string("Could not parse SPS!");
	}

	// Decode SPS from avcC.
	const uint8_t *p   = data;
	int            cnt = p[5] & 0x1f;   // Number of SPS.
	p += 6;
	if(cnt != 1) {
		cerr << "Not supporting more than 1 SPS unit for the moment; might fail horribly.\n";
	}
	for(int i = 0; i < cnt; i++) {
		//nalsize = AV_RB16(p) + 2;
		int nalsize = readBE<uint16_t>(p);
		if (p - data + nalsize > size) {
			throw string("Could not parse SPS!");
		}
#if 0
		// From: libavcodec/h264_parse.c
		ret = decode_extradata_ps_mp4(p, nalsize, ps, err_recognition, logctx);
		if (ret < 0) {
			av_log(logctx, AV_LOG_ERROR,
				   "Decoding SPS %d from avcC failed\n", i);
			return ret;
		}
		p += nalsize;
#endif
		break;
	}

	// Skip PPS.
}


class NalInfo {
	static const int MaxAVC1Length = 8 * (1 << 20);

public:
	int length;

	int ref_idc;
	int nal_type;
	int first_mb;           // Unused.
	int slice_type;         // Should match the NAL type (1, 5).
	int pps_id;             // Pic parameter set id: which parameter set to use.
	int frame_num;
	int field_pic_flag;
	int bottom_pic_flag;
	int idr_pic_flag;       // Actually 1 for nal_type 5, 0 for nal_type 0.
	int idr_pic_id;         // Read only for nal_type 5.
	int poc_type;           // If zero, check the poc lsb.
	int poc_lsb;            // Pic order count - least significant bit.

	NalInfo()
		: length(0),
		  ref_idc(0),
		  nal_type(0),
		  first_mb(0),
		  slice_type(0),
		  pps_id(0),
		  frame_num(0),
		  field_pic_flag(0),
		  bottom_pic_flag(0),
		  idr_pic_flag(0),
		  idr_pic_id(0),
		  poc_type(0),
		  poc_lsb(0)
	{ }

	bool getNalInfo(const H264sps &sps, uint32_t maxlength, const uint8_t *buffer);
	void clear();
	void print(int indentation = 0);

private:
	static int golomb  (       uint8_t *&buffer, int &offset);
	static int readBits(int n, uint8_t *&buffer, int &offset);
};


void NalInfo::clear() {
	length          = 0;
	ref_idc         = 0;
	nal_type        = 0;
	first_mb        = 0;
	slice_type      = 0;
	pps_id          = 0;
	frame_num       = 0;
	field_pic_flag  = 0;
	bottom_pic_flag = 0;
	idr_pic_flag    = 0;
	idr_pic_id      = 0;
	poc_type        = 0;
	poc_lsb         = 0;
}

void NalInfo::print(int indentation) {
	const string indent((indentation >= 0)? indentation : 0, ' ');

	cout << indent << "Length         : " << length          << ((length < 8+4 || length > MaxAVC1Length+4) ? " (incorrect)":"") << '\n';
	cout << indent << "Ref idc        : " << ref_idc         << ((unsigned(ref_idc) > 3) ? " (incorrect)":"") << '\n';
	cout << indent << "Nal type       : " << nal_type        << ((unsigned(nal_type) > 0x1f) ? " (incorrect)":"") << '\n';
	cout << indent << "First mb       : " << first_mb        << '\n';
	cout << indent << "Slice type     : " << slice_type      << ((unsigned(slice_type) > 9) ? " (incorrect)":"") << '\n';
	cout << indent << "Pic parm set id: " << pps_id          << '\n';
	cout << indent << "Frame number   : " << frame_num       << '\n';
	cout << indent << "Field  pic flag: " << field_pic_flag  << '\n';
	if(field_pic_flag)
		cout << indent << "Bottom pic flag: " << bottom_pic_flag << '\n';
	cout << indent << "Idr pic id     : " << idr_pic_id      << '\n';
	if(poc_type)
		cout << indent << "Poc type       : " << poc_type    << '\n';
	else
		cout << indent << "Poc lsb        : " << poc_lsb     << '\n';
	cout.flush();
}


int NalInfo::golomb(uint8_t *&buffer, int &offset) {
	assert(buffer != NULL && offset >= 0 && offset < 8);
	// Count the leading zeroes.
	int count = 0;
	while((*buffer & (0x01 << (7 - offset))) == 0) {
		count++;
		offset++;
		if(offset == 8) {
			buffer++;
			offset = 0;
		}
		if(count > 20) {
			cerr << "Failed reading golomb: too large!\n";
			return -1;
		}
	}
	// Skip the single 1 delimiter.
	offset++;
	if(offset == 8) {
		buffer++;
		offset = 0;
	}
	uint32_t res = 1;
	// Read count bits.
	while(count-- > 0) {
		res <<= 1;
		res |= (*buffer & (0x01 << (7 - offset))) >> (7 - offset);
		//res |= (*buffer >> (7 - offset)) & 0x01;
		offset++;
		if(offset == 8) {
			buffer++;
			offset = 0;
		}
	}
	return res - 1;
}

int NalInfo::readBits(int n, uint8_t *&buffer, int &offset) {
	assert(buffer != NULL && offset >= 0);
	int res = 0;
	// Can't read in a single reading.
	while(n + offset > 8) {
		int d = 8 - offset;
		res <<= d;
		res |= *buffer & ((1 << d) - 1);
		offset = 0;
		buffer++;
		n -= d;
	}
	// Read the remaining bits.
	int d = (8 - offset - n);
	res <<= n;
	res |= (*buffer >> d) & ((1 << n) - 1);
	return res;
}

// Return false means this probably is not a NAL.
bool NalInfo::getNalInfo(const H264sps &sps, uint32_t maxlength, const uint8_t *buffer) {
	// Re-initialize.
	clear();

	if(buffer[0] != 0) {
		cerr << "First byte expected 0.\n";
		return false;
	}

	// This is supposed to be the length of the NAL unit.
	uint32_t len = readBE<uint32_t>(buffer);
	if(len > MaxAVC1Length) {
		cerr << "Max length exceeded (" << len << " > " << MaxAVC1Length << ".\n";
		return false;
	}
	if(len + 4 > maxlength) {
		cerr << "Buffer size exceeded (" << (len + 4) << " > " << maxlength << ").\n";
		return false;
	}
	length = len + 4;
	cout << "Length         : " << length << '\n';

	buffer += 4;
	if(*buffer & (1 << 7)) {
		cerr << "Forbidden first bit 1.\n";
		return false;
	}
	ref_idc = *buffer >> 5;
	cout << "Ref idc        : " << ref_idc << '\n';

	nal_type = *buffer & 0x1f;
	cout << "Nal type       : " << nal_type << '\n';
	if(nal_type != 1 && nal_type != 5)
		return true;

	// Check if size is reasonable.
	if(len < 8) {
		cerr << "Length too short! (" << len << " < 8).\n";
		return false;
	}

	// Skip NAL header.
	buffer++;

	// Remove the emulation prevention 0x03 byte.
	// Could be done in place to speed up things.
	vector<uint8_t> data;
	data.reserve(len);
	for(unsigned int i = 0; i < len; i++) {
		data.push_back(buffer[i]);
		if(i+2 < len && buffer[i] == 0 && buffer[i+1] == 0 && buffer[i+2] == 3) {
			data.push_back(buffer[i+1]);
			assert(buffer[i+2] == 0x03);
			i += 2; // Skipping 3 byte!
		}
	}

	uint8_t *start  = data.data();
	int      offset = 0;
	first_mb   = golomb(start, offset);
	// TODO: Is there a max number, so we could validate?
	cout << "First mb       : " << first_mb << '\n';

	slice_type = golomb(start, offset);
	if(slice_type > 9) {
		cerr << "Invalid slice type (" << slice_type << "), probably this is not an avc1 sample.\n";
		return false;
	}
	cout << "Slice type     : " << slice_type << '\n';

	pps_id     = golomb(start, offset);
	cout << "Pic parm set id: " << pps_id << '\n';
	// pps id: should be taked from master context (h264_slice.c:1257).

	// Assume separate colour plane flag is 0,
	//  otherwise we would have to read colour_plane_id which is 2 bits.

	// Assuming same sps for all frames.
	//SPS *sps = reinterpret_cast<SPS *>(h->ps.sps_list[0]->data);  // may_alias.
	frame_num = readBits(sps.log2_max_frame_num, start, offset);
	cout << "Frame number   : " << frame_num << '\n';

	// Read 2 flags.
	field_pic_flag  = 0;
	bottom_pic_flag = 0;
	if(sps.frame_mbs_only_flag) {
		field_pic_flag = readBits(1, start, offset);
		cout << "Field  pic flag: " << field_pic_flag << '\n';
		if(field_pic_flag) {
			bottom_pic_flag = readBits(1, start, offset);
			cout << "Bottom pic flag: " << bottom_pic_flag << '\n';
		}
	}

	idr_pic_flag = (nal_type == 5) ? 1 : 0;
	if (idr_pic_flag) {
		idr_pic_id = golomb(start, offset);
		cout << "Idr pic id     : " << idr_pic_id << '\n';
	}

	// If the pic order count type == 0.
	poc_type = sps.poc_type;
	if(sps.poc_type == 0) {
		poc_lsb = readBits(sps.log2_max_poc_lsb, start, offset);
		cout << "Poc lsb        : " << poc_lsb << '\n';
	}

	// Ignoring the delta_poc for the moment.
	return true;
}



// Codec.
Codec::Codec() : context(NULL), codec(NULL), mask1(0), mask0(0) { }

void Codec::clear() {
	name.clear();
	// Do not remove the context, as it will be re-used!
	codec   = NULL;
	mask1   = 0;
	mask0   = 0;
}

bool Codec::parse(Atom *trak, vector<int> &offsets, Atom *mdat) {
	Atom *stsd = trak->atomByName("stsd");
	if(!stsd) {
		cerr << "Missing 'Sample Descriptions' atom (stsd).\n";
		return false;
	}
	int32_t entries = stsd->readInt(4);
	if(entries != 1)
		throw string("Multiplexed streams not supported");

	char codec_name[5];
	stsd->readChar(codec_name, 12, 4);
	name = codec_name;

	// This was a stupid attempt at trying to detect packet type based on bitmasks.
	mask1 = 0xffffffff;
	mask0 = 0xffffffff;
	// Build the mask:
	for(unsigned int i = 0; i < offsets.size(); i++) {
		uint32_t offset = offsets[i];
		if(offset < mdat->start || offset - mdat->start > mdat->length)
			throw string("Invalid offset in track!");

		int32_t s = mdat->readInt(offset - mdat->start - 8);
		mask1 &=  s;
		mask0 &= ~s;

		assert((s & mask1) == mask1);
		assert((~s & mask0) == mask0);
	}
	return true;
}


bool Codec::matchSample(const unsigned char *start, int maxlength) {
	int32_t s = readBE<int32_t>(start);

	if(name == "avc1") {
		// This works only for a very specific kind of video.
		//#define SPECIAL_VIDEO
#ifdef SPECIAL_VIDEO
		int32_t s2 = readBE<int32_t>(start + 4);
		if(s != 0x00000002 || (s2 != 0x09300000 && s2 != 0x09100000))
			return false;
		return true;
#endif

		// TODO: Use the first byte of the NAL: forbidden bit and type!
		int nal_type = (start[4] & 0x1f);
		// The other values are really uncommon on cameras...
		if(nal_type > 21) {
		//if(nal_type != 1 && !(nal_type >= 5 && nal_type <= 12)) {
#ifdef VERBOSE
			cout << "avc1: No match because of NAL type: " << nal_type << '\n';
#endif
			return false;
		}
		// If NAL is equal 7, the other fragments (starting with NAL type 7)
		//  should be part of the same packet.
		// (We cannot recover time information, remember.)
		if(start[0] == 0) {
#ifdef VERBOSE
			cout << "avc1: Match with 0 header.\n";
#endif
			return true;
		}
#ifdef VERBOSE
		cout << "avc1: Failed for no particular reason.\n";
#endif
		return false;

	} else if(name == "mp4a") {
		if(s > 1000000) {
#ifdef VERBOSE
			cout << "mp4a: Success because of large s value.\n";
#endif
			return true;
		}
		// XXX Horrible Hack: These values might need to be changed depending on the file. XXX
		if((start[4] == 0xee && start[5] == 0x1b) ||
		   (start[4] == 0x3e && start[5] == 0x64) )
		{
			cout << "mp4a: Success because of horrible hack.\n";
			return true;
		}

		if(start[0] == 0) {
#ifdef VERBOSE
			cout << "mp4a: Failure because of NULL header.\n";
#endif
			return false;
		}
#ifdef VERBOSE
		cout << "mp4a: Success for no particular reason....\n";
#endif
		return true;

#if 0 // THIS is true for mp3...
		// From: MP3'Tech Programmer's corner <http://www.mp3-tech.org/>.
		// MPEG Audio Layer I/II/III frame header (MSB->LSB):
		// BitPos   Length Use
		//  31-21  11 bits Frame sync [all bits are 1],
		//  20-19   2 bits MPEG Audio version Id [11=v1, 10=v2, 01=reserved, 00=v2.5+],
		//  18-17   2 bits Layer [11=I, 10=II, 01=III, 00=reserved],
		//     16   1 bit  Protection [1=unprotected, 0=16-bit CRC after this header],
		//  15-12   4 bits Bitrate index (variable for VBR) [0000=free, .., 1111=bad],
		//  11-10   2 bits Sampling rate frequency index [.., 11=reserved],
		//      9   1 bit  Padding (might be variable) [0=unpadded, 1=extra 32bit (L1) or 8bit (L2/L3)],
		//      8   1 bit  Private (???) [=0 ???],
		//   7- 6   2 bits Channel mode (constant) [00=Stereo, 01=Joint Stereo, 10=Dual Mono, 11=Single Mono],
		//   5- 4   2 bits Mode extension for Joint Stereo (variable),
		//      3   1 bit  Copyright [0=Not-Copyrighted, 1=Copyrighted],
		//      2   1 bit  Original [0=Copy, 1=Original Media],
		//   1- 0   2 bits Emphasis (variable) [00=None, 01=50/15 ms, 10=reserved, 11=CCIT J.17].
		// In practice we have:
		//  mask = 11111111111 11 11 1 0000 11 0 0 11 11 1 1 11
		//       = 1111 1111 1111 1111 0000 1100 1111 1111
		//       = 0xFFFF0CFF
		reverse(s);
		if(s & 0xFFE00000 != 0xFFE00000) // Check frame sync.
			return false;
		return true;
#endif

	} else if(name == "mp4v") {
		// As far as I know, keyframes are 1b3 and frames are 1b6 (ISO/IEC 14496-2, 6.3.4 6.3.5).
		if(s == 0x1b3 || s == 0x1b6)
			return true;
		return false;

	} else if(name == "alac") {
		int32_t t = readBE<int32_t>(start + 4);
		t &= 0xffff0000;

		if(s == 0      && t == 0x00130000) return true;
		if(s == 0x1000 && t == 0x001a0000) return true;
		return false;

	} else if(name == "samr") {
		return start[0] == 0x3c;

	} else if(name == "twos") {
		// Weird audio codec: each packet is 2 signed 16b integers.
		cerr << "The Twos audio codec is EVIL, there is no hope to guess it.\n";
		throw "Encountered an EVIL audio codec";
		return true;

	} else if(name == "apcn") {
		return memcmp(start, "icpf", 4) == 0;

	} else if(name == "lpcm") {
		// This is not trivial to detect, because it is just
		// the audio waveform encoded as signed 16-bit integers.
		// For now, just test that it is not "apcn" video.
		return memcmp(start, "icpf", 4) != 0;

	} else if(name == "in24") {
		// It's a codec id, in a case I found a pcm_s24le (little endian 24 bit).
		// No way to know it's length.
		return true;

	} else if(name == "sowt") {
		cerr << "Sowt is just raw data, no way to guess length (unless reliably detecting the other codec start).\n";
		return false;
	}

	return false;
}


int Codec::getLength(unsigned char *start, int maxlength, int &duration) {
	if(name == "mp4a") {
		if(!context)
			return -1;
		int consumed = -1;
		{
			AvLog useAvLog();
			AVFrame *frame = av_frame_alloc();
			if(!frame)
				throw string("Could not create AVFrame");
			AVPacket avp;
			av_init_packet(&avp);
			avp.data = start;
			avp.size = maxlength;
			int got_frame = 0;
			consumed = avcodec_decode_audio4(context, frame, &got_frame, &avp);
			if(consumed >= 0) {
				if(frame->nb_samples > 0)
					duration = frame->nb_samples;
				// Flush decoder to receive buffered packets.
				if(consumed <= 0 || duration <= 0) {
					clog << "Flush " << name << " decoder.\n";
					got_frame = 0;
					av_packet_unref(&avp);
					av_frame_unref(frame);
					int consumed2 = avcodec_decode_audio4(context, frame, &got_frame, &avp);
					if(consumed2 >= 0) {
						if(consumed <= 0)
							consumed = consumed2;
						if(duration <= 0 && frame->nb_samples > 0)
							duration = frame->nb_samples;
					}
				}
			}
			av_packet_unref(&avp);
			av_frame_free(&frame);
		}
		cout << "Duration: " << duration << '\n';
		return consumed;

	} else if(name == "mp4v") {
		if(!context)
			return -1;
		int consumed = -1;
		{
			AvLog useAvLog();
			AVFrame *frame = av_frame_alloc();
			if(!frame)
				throw string("Could not create AVFrame");
			AVPacket avp;
			av_init_packet(&avp);
			avp.data = start;
			avp.size = maxlength;
			int got_frame = 0;
			consumed = avcodec_decode_video2(context, frame, &got_frame, &avp);
			if(consumed == 0) {
				// Flush decoder to receive buffered packets.
				clog << "Flush " << name << " decoder.\n";
				got_frame = 0;
				av_packet_unref(&avp);
				av_frame_unref(frame);
				int consumed2 = avcodec_decode_video2(context, frame, &got_frame, &avp);
				if(consumed2 >= 0)
					consumed = consumed2;
			}
			av_packet_unref(&avp);
			av_frame_free(&frame);
		}
		return consumed;

	} else if(name == "avc1") {
		if(!context)
			return -1;

		// XXX: Horrible Hack: Referencing unstable, internal data structures. XXX
		H264Context *h    = static_cast<H264Context*>(context->priv_data); //context->codec->
		const SPS   *hsps = NULL;
		if(h) {
			// Use currently active SPS.
			//hsps = h->ps.sps;
			if(!hsps && h->ps.sps_list[0]) {
				// Use first SPS.
				hsps = reinterpret_cast<const SPS*>(h->ps.sps_list[0]->data); // may_alias.
			}
		}
		if(!hsps) {
			cerr << "Could not retrieve SPS.\n";
			//throw string("Could not retrieve SPS");
			return -1;
		}
		H264sps sps(*hsps);

#if 0
		int consumed = -1;
		{
			AvLog useAvLog();
			AVFrame *frame = av_frame_alloc();
			if(!frame)
				throw string("Could not create AVFrame");
			AVPacket avp;
			av_init_packet(&avp);
			avp.data = start;
			avp.size = maxlength;
			int got_frame = 0;
			consumed = avcodec_decode_video2(context, frame, &got_frame, &avp);
			if(consumed == 0) {
				// Flush decoder to receive buffered packets.
				clog << "Flush " << name << " decoder.\n";
				got_frame = 0;
				av_packet_unref(&avp);
				av_frame_unref(frame);
				int consumed2 = avcodec_decode_video2(context, frame, &got_frame, &avp);
				if(consumed2 >= 0)
					consumed = consumed2;
			}
			av_packet_unref(&avp);
			av_frame_free(&frame);
		}
		//clog << "Consumed: " << consumed << '\n';
		return consumed;
#endif

		// NAL unit types
		//  see: libavcodec/h264.h
		//  see: ITU-T T-REC-H.264-201704, Table 7-1
		// enum {
		//     NAL_SLICE             =  1,  // Non keyframe.
		//     NAL_DPA               =  2,
		//     NAL_DPB               =  3,
		//     NAL_DPC               =  4,
		//     NAL_IDR_SLICE         =  5,  // Keyframe.
		//     NAL_SEI               =  6,
		//     NAL_SPS               =  7,
		//     NAL_PPS               =  8,
		//     NAL_AUD               =  9,
		//     NAL_END_SEQUENCE      = 10,
		//     NAL_END_STREAM        = 11,
		//     NAL_FILLER_DATA       = 12,
		//     NAL_SPS_EXT           = 13,
		//     NAL_PREFIX            = 14,
		//     NAL_SUB_SPS           = 15,
		//     NAL_DPS               = 16,
		//     NAL_AUXILIARY_SLICE   = 19,
		//     NAL_EXTEN_SLICE       = 20,
		//     NAL_DEPTH_EXTEN_SLICE = 21,
		//
		//     NAL_FF_IGNORE         = 0xff0f001,
		// };
		//
		// First 4 bytes are the length, then the NAL starts.
		// ref_idc != 0 per unit_type = 5
		// ref_idc == 0 per unit_type = 6, 9, 10, 11, 12

		// See 7.4.1.2.4 Detection of the first VCL NAL unit of a primary coded picture
		//  for rules on how to group NALs into a picture.

		uint32_t             length = 0;
		const unsigned char *pos    = start;

		NalInfo previous;
		bool    seen_slice = false;

		while(true) {
			cout << '\n';
			NalInfo info;
			bool ok = info.getNalInfo(sps, maxlength, pos);
			if(!ok)
				return length;

			switch(info.nal_type) {
			case 1:
			case 5:
				if(!seen_slice) {
					previous   = info;
					seen_slice = true;
				} else {
					// Check for changes.
					if(previous.frame_num != info.frame_num) {
						cout << "Different frame number.\n";
						return length;
					}
					if(previous.pps_id != info.pps_id) {
						cout << "Different pic parameter set id.\n";
						return length;
					}
					// All these conditions are listed in the docs, but
					//   it looks like it creates invalid packets if respected.
					// Puzzling.
					//#define STRICT_NAL_INFO_CHECKING  1
#ifdef STRICT_NAL_INFO_CHECKING
					if(previous.field_pic_flag != info.field_pic_flag) {
						cout << "Different field  pic flag.\n";
						return length;
					}
					if(previous.field_pic_flag && info.field_pic_flag
					   && previous.bottom_pic_flag != info.bottom_pic_flag)
					{
						cout << "Different bottom pic flag.\n";
						return length;
					}
#endif
					if(previous.ref_idc != info.ref_idc) {
						cout << "Different ref idc.\n";
						return length;
					}
#ifdef STRICT_NAL_INFO_CHECKING
					if(previous.poc_type == 0 && info.poc_type == 0
					   && previous.poc_lsb != info.poc_lsb)
					{
						cout << "Different pic order count lsb (poc lsb).\n";
						return length;
					}
#endif
					if(previous.idr_pic_flag != info.idr_pic_flag) {
						cout << "Different NAL type (5, 1).\n";
					}
#ifdef STRICT_NAL_INFO_CHECKING
					if(previous.idr_pic_flag == 1 && info.idr_pic_flag == 1
					   && previous.idr_pic_id != info.idr_pic_id)
					{
						cout << "Different idr pic id for keyframe.\n";
						return length;
					}
#endif
				}
				break;
			default:
				if(seen_slice) {
					cerr << "New access unit since seen picture.\n";
					return length;
				}
				break;
			}
			pos       += info.length;
			length    += info.length;
			maxlength -= info.length;
			cout << "Partial length : " << length << '\n';
		}
		return length;

	} else if(name == "samr") {
		// Lenght is a multiple of 32, we split packets.
		return 32;

	} else if(name == "twos") {
		// Lenght is a multiple of 32, we split packets.
		return 4;

	} else if(name == "apcn") {
		return readBE<int32_t>(start);

	} else if(name == "lpcm") {
		// Use hard-coded values for now....
		const int num_samples      = 4096; // Empirical
		const int num_channels     =    2; // Stereo
		const int bytes_per_sample =    2; // 16-bit
		return num_samples * num_channels * bytes_per_sample;

	} else if(name == "in24") {
		return -1;

	} else
		return -1;
}

bool Codec::isKeyframe(const unsigned char *start, int maxlength) {
	if(name == "avc1") {
		// First byte of the NAL, the last 5 bits determine type
		//   (usually 5 for keyframe, 1 for intra frame).
		return (start[4] & 0x1F) == 5;
	} else
		return false;
}



// Track.
Track::Track() : trak(NULL), timescale(0), duration(0) { }

void Track::cleanUp() {
	trak      = NULL;
	timescale = 0;
	duration  = 0;
	offsets.clear();
	sizes.clear();
	keyframes.clear();
	times.clear();
	codec.clear();
}

bool Track::parse(Atom *t, Atom *mdat) {
	cleanUp();

	if(!t) {
		cerr << "Missing 'Container for an individual Track or stream' atom (trak).\n";
		return false;
	}
	trak = t;
	Atom *mdhd = t->atomByName("mdhd");
	if(!mdhd)
		throw string("Missing 'Media Header' atom (mdhd): Unknown duration and timescale");

	timescale = mdhd->readInt(12);
	duration  = mdhd->readInt(16);

	times     = getSampleTimes(t);
	keyframes = getKeyframes  (t);
	sizes     = getSampleSizes(t);

	vector<int> chunk_offsets   = getChunkOffsets(t);
	vector<int> sample_to_chunk = getSampleToChunk(t, chunk_offsets.size());

	if(times.size() != sizes.size()) {
		clog << "Mismatch between time offsets and size offsets.\n";
		clog << "Time offsets: " << times.size() << " Size offsets: " << sizes.size() << '\n';
	}
	//assert(times.size() == sizes.size());
	if(times.size() != sample_to_chunk.size()) {
		clog << "Mismatch between time offsets and sample_to_chunk offsets.\n";
		clog << "Time offsets: " << times.size() << " Chunk offsets: " << sample_to_chunk.size() << '\n';
	}
	// Compute actual offsets.
	int old_chunk = -1;
	int offset = -1;
	for(unsigned int i = 0; i < sizes.size(); i++) {
		int chunk = sample_to_chunk[i];
		int size = sizes[i];
		if(chunk != old_chunk) {
			offset = chunk_offsets[chunk];
			old_chunk= chunk;
		}
		offsets.push_back(offset);
		offset += size;
	}

	// Move this stuff into track!
	Atom *hdlr = trak->atomByName("hdlr");
	if(!hdlr) {
		cerr << "Missing 'Handler' atom (hdlr).\n";
		return false;
	}
	char type[5];
	hdlr->readChar(type, 8, 4);

	if(type != string("soun") && type != string("vide")) {
		clog << "Not an Audio nor Video track.\n";
		return true;
	}
	// If audio, use next?
	//bool audio = (type == string("soun"));

	// Move this to Codec.
	codec.parse(trak, offsets, mdat);
	if(!codec.context)
		throw string("No codec context.");
	{
		AvLog useAvLog();
		codec.codec = avcodec_find_decoder(codec.context->codec_id);
		if(!codec.codec)
			throw string("No codec found!");
		if(avcodec_open2(codec.context, codec.codec, NULL) < 0) {
			throw string("Could not open codec: ")
				+ ((codec.context->codec && codec.context->codec->name)? codec.context->codec->name : "???");
		}
	}

#if 0
	if(!mdat)
		throw string("Missing 'Media Data container' atom (mdat)");

	// Print sizes and offsets.
	clog << "Track codec: " << codec.name << '\n';
	clog << "Sizes      : " << sizes.size() << '\n';
	for(unsigned int i = 0; i < 10 && i < sizes.size(); i++) {
		int64_t offset = offsets[i] - (mdat->start + 8);
		int64_t begin  = mdat->readInt(offset);
		int64_t next   = mdat->readInt(offset + 4);
		int64_t end    = mdat->readInt(offset + sizes[i] - 4);
		// Use <iomanip> for layout.
		clog << setw(8) << i
			<< " Size: " << setw(6) << sizes[i]
			<< " offset " << setw(10) << offsets[i]
			<< "  begin: " << hex << setw(5) << begin << ' ' << setw(8) << next
			<< " end: " << setw(8) << end << dec << '\n';
	}
	if(sizes.size() > 10)
		clog << "...\n";
	clog << endl;
#endif
	return true;
}

void Track::writeToAtoms() {
	if(!trak)
		return;

	if(keyframes.empty())
		trak->prune("stss");

	saveSampleTimes();
	saveKeyframes();
	saveSampleSizes();
	saveSampleToChunk();
	saveChunkOffsets();

	Atom *mdhd = trak->atomByName("mdhd");
	if(!mdhd)
		cerr << "Missing 'Media Header' atom (mdhd).\n";
	else
		mdhd->writeInt(duration, 16);

	// Avc1 codec writes something inside stsd.
	// In particular the picture parameter set (PPS) in avcC (after the sequence parameter set).
	// For avcC see: <http://jaadec.sourceforge.net/specs/ISO_14496-15_AVCFF.pdf>.
	// For PPS  see: <https://www.iitk.ac.in/mwn/vaibhav/Vaibhav%20Sharma_files/h.264_standard_document.pdf>.
	// For PPS  see: <http://last.hit.bme.hu/download/firtha/video/h264/Iain%20E.%20Richardson%20-%20H264%20(2nd%20edition).pdf>.
	// I have found out in shane,banana,bruno and nawfel that pic_init_qp_minus26 is different
	//  even for consecutive videos of the same camera.
	// The only thing to do then is to test possible values,
	//  to do so remove 28 (the NAL header) follow the golomb stuff.
	// This test could be done automatically when decoding fails..
	//#define SHANE 6
#ifdef SHANE
	int pps[15] = {
		0x28ee3880,
		0x28ee1620,
		0x28ee1e20,
		0x28ee0988,
		0x28ee0b88,
		0x28ee0d88,
		0x28ee0f88,
		0x28ee0462,
		0x28ee04e2,
		0x28ee0562,
		0x28ee05e2,
		0x28ee0662,
		0x28ee06e2,
		0x28ee0762,
		0x28ee07e2
	};
	if(codec.name == "avc1") {
		Atom *stsd = trak->atomByName("stsd");
		if(stsd)
			stsd->writeInt(pps[SHANE], 122); //a bit complicated to find.... find avcC... follow first link.
	}
#endif

}

void Track::clear() {
	offsets.clear();
	sizes.clear();
	keyframes.clear();
	//times.clear();
}

void Track::fixTimes() {
	if(codec.name == "samr") {
		times.clear();
		times.resize(offsets.size(), 160);
		return;
	}
	while(times.size() < offsets.size())
		times.insert(times.end(), times.begin(), times.end());
	times.resize(offsets.size());

	duration = 0;
	for(unsigned int i = 0; i < times.size(); i++)
		duration += times[i];
}


vector<int> Track::getSampleTimes(Atom *t) {
	assert(t != NULL);
	vector<int> sample_times;
	// Chunk offsets.
	Atom *stts = t->atomByName("stts");
	if(!stts)
		throw string("Missing 'Sync Sample Table' atom (stts)");

	int32_t entries = stts->readInt(4);
	for(int i = 0; i < entries; i++) {
		int32_t nsamples = stts->readInt( 8 + 8*i);
		int32_t time     = stts->readInt(12 + 8*i);
		for(int i = 0; i < nsamples; i++)
			sample_times.push_back(time);
	}
	return sample_times;
}

vector<int> Track::getKeyframes(Atom *t) {
	assert(t != NULL);
	vector<int> sample_key;
	// Chunk offsets.
	Atom *stss = t->atomByName("stss");
	if(!stss)
		return sample_key;

	int32_t entries = stss->readInt(4);
	for(int i = 0; i < entries; i++)
		sample_key.push_back(stss->readInt(8 + 4*i) - 1);
	return sample_key;
}

vector<int> Track::getSampleSizes(Atom *t) {
	assert(t != NULL);
	vector<int> sample_sizes;
	// Chunk offsets.
	Atom *stsz = t->atomByName("stsz");
	if(!stsz)
		throw string("Missing 'Sample Sizes' atom (stsz)");

	int32_t entries      = stsz->readInt(8);
	int32_t default_size = stsz->readInt(4);
	if(default_size == 0) {
		for(int i = 0; i < entries; i++)
			sample_sizes.push_back(stsz->readInt(12 + 4*i));
	} else {
		sample_sizes.resize(entries, default_size);
	}
	return sample_sizes;
}

vector<int> Track::getChunkOffsets(Atom *t) {
	assert(t != NULL);
	vector<int> chunk_offsets;
	// Chunk offsets.
	Atom *stco = t->atomByName("stco");
	if(stco) {
		int32_t nchunks = stco->readInt(4);
		for(int i = 0; i < nchunks; i++)
			chunk_offsets.push_back(stco->readInt(8 + i*4));

	} else {
		Atom *co64 = t->atomByName("co64");
		if(!co64)
			throw string("Missing both 'Chunk Offset' atoms (stco & co64)");

		int32_t nchunks = co64->readInt(4);
		for(int i = 0; i < nchunks; i++) {
			int32_t hi32 = co64->readInt( 8 + i*8); //high order 32-bits
			int32_t lo32 = co64->readInt(12 + i*8); //low  order 32-bits
			if(hi32 != 0) {
				cerr << "Overflow: 64-bit Chunk Offset value too large ("
					<< ((int64_t(hi32) << 32) | uint32_t(lo32)) << ").\n";
			}
			chunk_offsets.push_back(lo32);
		}
	}
	return chunk_offsets;
}

vector<int> Track::getSampleToChunk(Atom *t, int nchunks){
	assert(t != NULL);
	vector<int> sample_to_chunk;

	Atom *stsc = t->atomByName("stsc");
	if(!stsc)
		throw string("Missing 'Sample to Chunk' atom (stsc)");

	vector<int> first_chunks;
	int32_t entries = stsc->readInt(4);
	for(int i = 0; i < entries; i++)
		first_chunks.push_back(stsc->readInt(8 + 12*i));
	first_chunks.push_back(nchunks+1);

	for(int i = 0; i < entries; i++) {
		int first_chunk = first_chunks[i];
		int last_chunk  = first_chunks[i + 1];
		int32_t nsamples = stsc->readInt(12 + 12*i);

		for(int k = first_chunk; k < last_chunk; k++) {
			for(int j = 0; j < nsamples; j++)
				sample_to_chunk.push_back(k - 1);
		}
	}

	return sample_to_chunk;
}


void Track::saveSampleTimes() {
	if(!trak)
		return;
	Atom *stts = trak->atomByName("stts");
	assert(stts);
	if(!stts)
		return;
	stts->content.resize(4 +                //version
						 4 +                //entries
						 8*times.size());   //time table
	stts->writeInt(times.size(), 4);
	for(unsigned int i = 0; i < times.size(); i++) {
		stts->writeInt(1, 8 + 8*i);
		stts->writeInt(times[i], 12 + 8*i);
	}
}

void Track::saveKeyframes() {
	if(!trak)
		return;
	Atom *stss = trak->atomByName("stss");
	if(!stss)
		return;
	assert(keyframes.size() > 0);
	if(keyframes.empty())
		return;

	stss->content.resize(4 +                  //version
						 4 +                  //entries
						 4*keyframes.size()); //time table
	stss->writeInt(keyframes.size(), 4);
	for(unsigned int i = 0; i < keyframes.size(); i++)
		stss->writeInt(keyframes[i] + 1, 8 + 4*i);
}

void Track::saveSampleSizes() {
	if(!trak)
		return;
	Atom *stsz = trak->atomByName("stsz");
	assert(stsz);
	if(!stsz)
		return;
	stsz->content.resize(4 +                //version
						 4 +                //default size
						 4 +                //entries
						 4*sizes.size());   //size table
	stsz->writeInt(0, 4);
	stsz->writeInt(sizes.size(), 8);
	for(unsigned int i = 0; i < sizes.size(); i++)
		stsz->writeInt(sizes[i], 12 + 4*i);
}

void Track::saveSampleToChunk() {
	if(!trak)
		return;
	Atom *stsc = trak->atomByName("stsc");
	assert(stsc);
	if(!stsc)
		return;
	stsc->content.resize(4 +                //version
						 4 +                //number of entries
						 12);               //one sample per chunk.
	stsc->writeInt(1,  4);
	stsc->writeInt(1,  8);                  //first chunk (1 based)
	stsc->writeInt(1, 12);                  //one sample per chunk
	stsc->writeInt(1, 16);                  //id 1 (WHAT IS THIS!)
}

void Track::saveChunkOffsets() {
	if(!trak)
		return;
	Atom *co64 = trak->atomByName("co64");
	if(co64) {
		trak->prune("co64");
		Atom *stbl = trak->atomByName("stbl");
		if(stbl) {
			Atom *new_stco = new Atom;
			memcpy(new_stco->name, "stco", min(sizeof("stco"), sizeof(new_stco->name)-1));
			stbl->children.push_back(new_stco);
		}
	}
	Atom *stco = trak->atomByName("stco");
	assert(stco);
	if(!stco)
		return;
	stco->content.resize(4 +                //version
						 4 +                //number of entries
						 4*offsets.size());
	stco->writeInt(offsets.size(), 4);
	for(unsigned int i = 0; i < offsets.size(); i++)
		stco->writeInt(offsets[i], 8 + 4*i);
}

// vim:set ts=4 sw=4 sts=4 noet:
