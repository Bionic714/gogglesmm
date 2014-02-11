/*******************************************************************************
*                         Goggles Audio Player Library                         *
********************************************************************************
*           Copyright (C) 2010-2014 by Sander Jansen. All Rights Reserved      *
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

#ifdef HAVE_MP4_PLUGIN
#include "mp4ff.h"
#endif


namespace ap {



enum {
  AAC_FLAG_CONFIG = 0x2,
  AAC_FLAG_FRAME  = 0x4
  };

#ifdef HAVE_MP4_PLUGIN

class MP4Reader : public ReaderPlugin {
protected:
  mp4ff_callback_t callback;
  mp4ff_t*         handle;
protected:
  static FXuint mp4_read(void*,void*,FXuint);
  static FXuint mp4_write(void*,void*,FXuint);
  static FXuint mp4_seek(void*,FXulong);
	static FXuint mp4_truncate(void*);
protected:
  Packet * packet;
  FXlong   datastart;
  FXint    nframes;
  FXint    frame;
  FXint    track;
protected:
  ReadStatus parse();
  void send_meta();
public:
  MP4Reader(AudioEngine*);
  FXuchar format() const { return Format::MP4; };
  FXbool init(InputPlugin*);
  FXbool can_seek() const;
  FXbool seek(FXdouble);
  ReadStatus process(Packet*);
  ~MP4Reader();
  };


FXuint MP4Reader::mp4_read(void*ptr,void*data,FXuint len){
  InputPlugin* input = reinterpret_cast<InputPlugin*>(ptr);
  return (FXuint) input->read(data,len);
  }

FXuint MP4Reader::mp4_write(void*,void*,FXuint){
  FXASSERT(0);
//  InputThread* input = reinterpret_cast<InputThread*>(ptr);
  return 0;
  }

FXuint MP4Reader::mp4_seek(void*ptr,FXulong p){
  InputPlugin* input = reinterpret_cast<InputPlugin*>(ptr);
  return input->position(p,FXIO::Begin);
  //return input->position();
  }

FXuint MP4Reader::mp4_truncate(void*){
  FXASSERT(0);
  //InputThread* input = reinterpret_cast<InputThread*>(ptr);
  return 0;
  }



MP4Reader::MP4Reader(AudioEngine* e) : ReaderPlugin(e),handle(NULL),nframes(-1),track(-1) {
  }

MP4Reader::~MP4Reader(){
  if (handle) mp4ff_close(handle);
  }

FXbool MP4Reader::init(InputPlugin*plugin) {
  ReaderPlugin::init(plugin);

  if (handle) {
    mp4ff_close(handle);
    handle=NULL;
    }

  callback.read      = mp4_read;
  callback.write     = mp4_write;
  callback.seek      = mp4_seek;
  callback.truncate  = mp4_truncate;
  callback.user_data = input;

  track=-1;
  frame=0;
  nframes=-1;

  flags&=~FLAG_PARSED;
  return true;
  }


FXbool MP4Reader::can_seek() const {
  return true;
  }

FXbool MP4Reader::seek(FXdouble pos){
  FXint f = mp4ff_find_sample(handle,track,pos*stream_length,NULL);
  if (f>=0) frame=f;
  return true;
  }

void MP4Reader::send_meta() {
  FXchar * value;
  MetaInfo * meta = new MetaInfo();
  if (mp4ff_meta_get_title(handle,&value)) {
    meta->title = value;
    free(value);
    }
  if (mp4ff_meta_get_artist(handle,&value)) {
    meta->artist = value;
    free(value);
    }

  if (mp4ff_meta_get_album(handle,&value)) {
    meta->album = value;
    free(value);
    }
  engine->decoder->post(meta);
  }



ReadStatus MP4Reader::process(Packet*p) {
  packet=p;
  packet->stream_position=-1;
  packet->stream_length=stream_length;
  packet->flags=AAC_FLAG_FRAME;

  if (!(flags&FLAG_PARSED)) {
    return parse();
    }

  packet->stream_position = mp4ff_get_sample_position(handle,track,frame);

  for (;frame<nframes;frame++) {
    FXint size = mp4ff_read_sample_getsize(handle,track,frame);

    if (size<=0) {
      packet->unref();
      return ReadError;
      }

    if (packet->space()<size) {
      if (packet->size()) {
        engine->decoder->post(packet);
        packet=NULL;
        }
      return ReadOk;
      }

    FXint n = mp4ff_read_sample_v2(handle,track,frame,packet->ptr());
    if (n<=0) {
      packet->unref();
      return ReadError;
      }
    packet->wroteBytes(size);


    if (frame==nframes-1)
      packet->flags|=FLAG_EOS;
    }

  if (packet->size()) {
    engine->decoder->post(packet);
    packet=NULL;
    return ReadDone;
    }
  return ReadOk;
  }



ReadStatus MP4Reader::parse() {
  mp4AudioSpecificConfig cfg;
  FXuchar* buffer;
  FXuint   size;
  FXint    ntracks;

  FXASSERT(handle==NULL);
  FXASSERT(packet);

  handle = mp4ff_open_read(&callback);
  if (handle==NULL)
    goto error;

  ntracks = mp4ff_total_tracks(handle);
  if (ntracks<=0)
    goto error;

  for (FXint i=0;i<ntracks;i++) {
    if ((mp4ff_get_decoder_config(handle,i,&buffer,&size)==0) && buffer && size) {
      if (NeAACDecAudioSpecificConfig(buffer,size,&cfg)==0) {

        af.set(AP_FORMAT_S16,mp4ff_get_sample_rate(handle,i),mp4ff_get_channel_count(handle,i));
        af.debug();

        if (size>packet->space()) {
          GM_DEBUG_PRINT("MP4 config buffer is too big for decoder packet");
          free(buffer);
          goto error;
          }

        track=i;
        frame=0;
        nframes=mp4ff_num_samples(handle,i);
        stream_length=mp4ff_get_track_duration(handle,i);


        packet->append(buffer,size);
        packet->flags|=AAC_FLAG_CONFIG|AAC_FLAG_FRAME;
        engine->decoder->post(new ConfigureEvent(af,Codec::AAC));

        send_meta();

        engine->decoder->post(packet);

        packet=NULL;
        flags|=FLAG_PARSED;
        free(buffer);
        return ReadOk;
        }
      free(buffer);
      }
    }

error:
  packet->unref();
  return ReadError;
  }



ReaderPlugin * ap_mp4_reader(AudioEngine * engine) {
  return new MP4Reader(engine);
  }

#endif


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
    GM_DEBUG_PRINT("finding sync\n");
    FXuchar buffer[2];
    if (input->read(buffer,2)!=2)
      return ReadError;
    do {
      if ((buffer[0]==0xFF) && (buffer[1]&0xf0)==0xf0) {
        GM_DEBUG_PRINT("found sync\n");
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
  FXlong         position;
protected:
  Packet * in;
  Packet * out;
public:
  AacDecoder(AudioEngine*);
  FXuchar codec() const { return Codec::AAC; }
  FXbool flush();
  FXbool init(ConfigureEvent*);
  DecoderStatus process(Packet*);
  ~AacDecoder();
  };



AacDecoder::AacDecoder(AudioEngine * e) : DecoderPlugin(e),handle(NULL),position(-1),in(NULL),out(NULL) {
  }

AacDecoder::~AacDecoder() {
  flush();
  }

FXbool AacDecoder::init(ConfigureEvent*event) {
  af=event->af;
  if (handle) {
    NeAACDecClose(handle);
    handle=NULL;
    }
  return true;
  }

FXbool AacDecoder::flush() {
  if (out) {
    buffer.clear();
    out->clear();
    }
  return true;
  }

DecoderStatus AacDecoder::process(Packet*packet){
  FXbool eos = packet->flags&FLAG_EOS;

  if (packet->flags&AAC_FLAG_FRAME)
    position = packet->stream_position;

  long unsigned int samplerate;
  FXuchar           channels;
  NeAACDecFrameInfo frame;

  if (packet->flags&AAC_FLAG_CONFIG) {
    handle = NeAACDecOpen();
    if (NeAACDecInit2(handle,packet->data(),packet->size(),&samplerate,&channels)<0){
      packet->unref();
      return DecoderError;
      }
    return DecoderOk;
    }
  else {
    buffer.append(packet->data(),packet->size());
    packet->unref();
    if (handle==NULL) {
      handle = NeAACDecOpen();
      long n = NeAACDecInit(handle,buffer.data(),buffer.size(),&samplerate,&channels);
      if (n<0) return DecoderError;
      else if (n>0) buffer.readBytes(n);
      af.set(AP_FORMAT_S16,samplerate,channels);
      engine->output->post(new ConfigureEvent(af,Codec::AAC));
      position=0;
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
      out->stream_position = position;
      out->stream_length   = packet->stream_length;
      }

    void * outbuffer = out->ptr();
    NeAACDecDecode2(handle,&frame,buffer.data(),buffer.size(),&outbuffer,out->availableFrames()*out->af.framesize());
    if (frame.bytesconsumed>0) {
      buffer.readBytes(frame.bytesconsumed);
      }

	  if (frame.error > 0) {
	    GM_DEBUG_PRINT("[aac] error %d (%ld): %s\n",frame.error,frame.bytesconsumed,faacDecGetErrorMessage(frame.error));
	    }

    if (frame.samples) {
      position+=(frame.samples/frame.channels);
      out->wroteFrames((frame.samples/frame.channels));
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
    engine->output->post(new ControlEvent(End,packet->stream));
    }

  return DecoderOk;
  }




DecoderPlugin * ap_aac_decoder(AudioEngine * engine) {
  return new AacDecoder(engine);
  }

#endif
}


