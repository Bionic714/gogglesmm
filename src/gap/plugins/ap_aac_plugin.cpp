/*******************************************************************************
*                         Goggles Audio Player Library                         *
********************************************************************************
*           Copyright (C) 2010-2015 by Sander Jansen. All Rights Reserved      *
*                               ---                                            *
* This program is free software: you can redistribute it and/or modify         *
* it under the terms of the GNU General Public License as published by         *
* the Free Software Foundation, either version 3 of the License, or            *
* (at your option) any later version.                                          *
*                                                                              *
* This program is distributed in the hope that it will be useful,              *
* but WITHOUT ANY WARRANTY; without even the implied warranty of               *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                *
* GNU General Public License for more details.                                 *
*                                                                              *
* You should have received a copy of the GNU General Public License            *
* along with this program.  If not, see http://www.gnu.org/licenses.           *
********************************************************************************/
#include "ap_defs.h"
#include "ap_config.h"
#include "ap_pipe.h"
#include "ap_format.h"
#include "ap_device.h"
#include "ap_event.h"
#include "ap_reactor.h"
#include "ap_event_private.h"
#include "ap_buffer.h"
#include "ap_packet.h"
#include "ap_event_queue.h"
#include "ap_thread_queue.h"
#include "ap_engine.h"
#include "ap_reader_plugin.h"
#include "ap_input_plugin.h"
#include "ap_decoder_plugin.h"
#include "ap_thread.h"
#include "ap_input_thread.h"
#include "ap_decoder_thread.h"
#include "ap_output_thread.h"


#include "neaacdec.h"

namespace ap {

enum {
  AAC_FLAG_CONFIG = 0x2,
  AAC_FLAG_FRAME  = 0x4
  };

#ifdef HAVE_AAC_PLUGIN

class AACReader : public ReaderPlugin {
public:
  AACReader(AudioEngine*e) : ReaderPlugin(e) {}
  FXbool init(InputPlugin*plugin) { ReaderPlugin::init(plugin); flags=0; return true; }
  FXuchar format() const { return Format::AAC; }

  ReadStatus process(Packet*p);

  ~AACReader() {}
  };

ReaderPlugin * ap_aac_reader(AudioEngine * engine) {
  return new AACReader(engine);
  }

ReadStatus AACReader::process(Packet*packet) {
  if (!(flags&FLAG_PARSED)) {
    GM_DEBUG_PRINT("[aac] finding sync\n");
    FXuchar buffer[2];
    if (input->read(buffer,2)!=2)
      return ReadError;
    do {
      if ((buffer[0]==0xFF) && (buffer[1]&0xf0)==0xf0) {
        GM_DEBUG_PRINT("[aac] found sync\n");
        engine->decoder->post(new ConfigureEvent(af,Codec::AAC));
        flags|=FLAG_PARSED;
        packet->append(buffer,2);
        break;
        }
      buffer[0]=buffer[1];
      if (input->read(&buffer[1],1)!=1)
        return ReadError;
      }
    while(1);
    }
  return ReaderPlugin::process(packet);
  }











class AacDecoder : public DecoderPlugin {
protected:
  NeAACDecHandle handle;
  MemoryBuffer   buffer;
  FXlong         stream_position;
protected:
  Packet * out;
public:
  AacDecoder(AudioEngine*);
  FXuchar codec() const { return Codec::AAC; }
  FXbool flush(FXlong offset=0);
  FXbool init(ConfigureEvent*);
  DecoderStatus process(Packet*);
  ~AacDecoder();
  };



AacDecoder::AacDecoder(AudioEngine * e) : DecoderPlugin(e),handle(NULL),stream_position(-1),out(NULL) {
  }

AacDecoder::~AacDecoder() {
  flush();
  if (handle) {
    NeAACDecClose(handle);
    handle=NULL;
    }
  }

FXbool AacDecoder::init(ConfigureEvent*event) {
  DecoderPlugin::init(event);
  buffer.clear();
  af=event->af;
  if (handle) {
    NeAACDecClose(handle);
    handle=NULL;
    }
  stream_position=-1;
  return true;
  }

FXbool AacDecoder::flush(FXlong offset) {
  DecoderPlugin::flush(offset);
  buffer.clear();
  if (out) {
    out->unref();
    out=NULL;
    }
  stream_position=-1;
  return true;
  }

DecoderStatus AacDecoder::process(Packet*packet){
  const FXbool eos = packet->flags&FLAG_EOS;
  const FXlong stream_length = packet->stream_length;
  const FXuint stream_id = packet->stream;

  if (stream_position==-1 && buffer.size()==0)
    stream_position = packet->stream_position;

  long unsigned int samplerate;
  FXuchar           channels;
  NeAACDecFrameInfo frame;

  if (packet->flags&AAC_FLAG_CONFIG) {
    FXASSERT(handle==NULL);
    handle = NeAACDecOpen();
    if (NeAACDecInit2(handle,packet->data(),packet->size(),&samplerate,&channels)<0){
      packet->unref();
      packet=NULL;
      return DecoderError;
      }
    packet->unref();
    packet=NULL;
    return DecoderOk;
    }
  else {
    buffer.append(packet->data(),packet->size());
    packet->unref();
    packet=NULL;
    if (handle==NULL) {
      handle = NeAACDecOpen();
      long n = NeAACDecInit(handle,buffer.data(),buffer.size(),&samplerate,&channels);
      if (n<0) {
        buffer.clear();
        return DecoderError;
        }
      else if (n>0) buffer.readBytes(n);
      af.set(AP_FORMAT_S16,samplerate,channels);
      engine->output->post(new ConfigureEvent(af,Codec::AAC));
      stream_position=0;
      }
    }

  if (buffer.size()<FAAD_MIN_STREAMSIZE*2) {
    return DecoderOk;
    }

  do {

    if (out==NULL){
      out = engine->decoder->get_output_packet();
      if (out==NULL) return DecoderInterrupted;
      out->af              = af;
      out->stream          = stream_id;
      out->stream_position = stream_position;
      out->stream_length   = stream_length;
      }

    void * outbuffer = out->ptr();
    NeAACDecDecode2(handle,&frame,buffer.data(),buffer.size(),&outbuffer,out->availableFrames()*out->af.framesize());
    if (frame.bytesconsumed>0) {
      buffer.readBytes(frame.bytesconsumed);
      }

    if (frame.error > 0) {
      GM_DEBUG_PRINT("[aac] error %d (%ld): %s\n",frame.error,frame.bytesconsumed,faacDecGetErrorMessage(frame.error));//
      }

    if (frame.samples) {
      const FXint nframes = frame.samples / frame.channels;
      if (__unlikely(stream_position<stream_decode_offset)) {
        if ((nframes+stream_position)<stream_decode_offset) {
          GM_DEBUG_PRINT("[aac] stream decode offset %ld. Full skip %d\n",stream_decode_offset,nframes);
          stream_position+=nframes;
          }
        else {
          GM_DEBUG_PRINT("[aac] stream decode offset %ld. Partial skip %ld\n",stream_decode_offset,(stream_decode_offset-stream_position));
          out->wroteFrames(nframes);
          out->trimBegin(af.framesize()*(stream_decode_offset-stream_position));
          out->stream_position = stream_decode_offset;
          stream_position+=nframes;
          }
        }
      else {
        stream_position+=nframes;
        out->wroteFrames(nframes);
        }
      if (out->availableFrames()==0) {
        engine->output->post(out);
        out=NULL;
        }
      }
    }
  while(buffer.size()>(2*FAAD_MIN_STREAMSIZE) && frame.bytesconsumed);

  if (eos) {
    if (out) {
      engine->output->post(out);
      out=NULL;
      }
    engine->output->post(new ControlEvent(End,stream_id));
    }

  return DecoderOk;
  }




DecoderPlugin * ap_aac_decoder(AudioEngine * engine) {
  return new AacDecoder(engine);
  }

#endif
}


