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
#include "ap_common.h"
#include "ap_pipe.h"
#include "ap_format.h"
#include "ap_device.h"
#include "ap_event.h"
#include "ap_event_private.h"
#include "ap_event_queue.h"
#include "ap_thread_queue.h"
#include "ap_buffer.h"
#include "ap_packet.h"
#include "ap_engine.h"
#include "ap_reader_plugin.h"
#include "ap_decoder_plugin.h"
#include "ap_thread.h"
#include "ap_input_thread.h"
#include "ap_buffer.h"
#include "ap_decoder_thread.h"
#include "ap_xml_parser.h"

namespace ap {


class XSPFParser : public XmlParser{
public:
  FXStringList files;
  FXString     title;
protected:
  FXint        elem;
protected:
  FXint begin(const FXchar *,const FXchar**);
  void data(const FXchar *,FXint len);
  void end(const FXchar *);
public:
  enum {
    Elem_Playlist = Elem_Last,
    Elem_Playlist_Title,
    Elem_Playlist_TrackList,
    Elem_Playlist_TrackList_Track,
    Elem_Playlist_TrackList_Track_Location,
    };
public:
  XSPFParser();
  ~XSPFParser();
  };

XSPFParser::XSPFParser() : elem(Elem_None) {
  }

XSPFParser::~XSPFParser(){
  }

FXint XSPFParser::begin(const FXchar * element,const FXchar **/* attributes*/){
  switch(node()) {
    case Elem_None:
      {
        if (compare(element,"playlist")==0)
          return Elem_Playlist;
      } break;
    case Elem_Playlist:
      {
        if (compare(element,"title")==0)
          return Elem_Playlist_Title;
        else if (compare(element,"trackList")==0)
          return Elem_Playlist_TrackList;

      } break;
    case Elem_Playlist_TrackList:
      {
        if (compare(element,"track")==0)
          return Elem_Playlist_TrackList_Track;

      } break;
    case Elem_Playlist_TrackList_Track:
      {
        if (compare(element,"location")==0)
          return Elem_Playlist_TrackList_Track_Location;
      } break;
    default: return 0; // skip
    }
  return 0;
  }


void XSPFParser::data(const FXchar* str,FXint len){
  switch(node()) {
    case Elem_Playlist_Title:  title.assign(str,len); break;
    case Elem_Playlist_TrackList_Track_Location:
      {
        FXString url(str,len);
        files.append(url);

      } break;
    }
  }

void XSPFParser::end(const FXchar*) {
  }

void ap_parse_xspf(const FXString & data,FXStringList & mrl,FXString & title) {
  XSPFParser xspf;
  if (xspf.parse(data)) {
    mrl=xspf.files;
    title=xspf.title;
    }
  }


class XSPFReader : public TextReader {
protected:
  FXStringList uri;
public:
  XSPFReader(AudioEngine*);
  ReadStatus process(Packet*);
  FXbool init(InputPlugin*);
  FXuchar format() const { return Format::XSPF; };
  FXbool redirect(FXStringList & u) { u=uri; return true; }
  virtual ~XSPFReader();
  };


XSPFReader::XSPFReader(AudioEngine*e) : TextReader(e) {
  }

XSPFReader::~XSPFReader(){
  }

FXbool XSPFReader::init(InputPlugin*plugin) {
  TextReader::init(plugin);
  uri.clear();
  return true;
  }

ReadStatus XSPFReader::process(Packet*packet) {
  if (TextReader::process(packet)==ReadDone) {
    FXString title;
    ap_parse_xspf(textbuffer,uri,title);
    if (uri.no())
      return ReadRedirect;
    else
      return ReadDone;
    }
  return ReadError;
  }

ReaderPlugin * ap_xspf_reader(AudioEngine * engine) {
  return new XSPFReader(engine);
  }


}
