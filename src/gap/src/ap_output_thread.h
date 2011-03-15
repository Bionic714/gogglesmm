#ifndef OUTPUT_H
#define OUTPUT_H

class AudioEngine;
class OutputPlugin;
class Packet;

class OutputThread : public EngineThread {
protected:
  Event * wait_for_packet();
  Event * wait_for_event();
protected:
  FXString       output_config;
public:
  AudioFormat    af;
  OutputPlugin * plugin;
  FXDLL          dll;
  MemoryBuffer   converted_samples;
  MemoryBuffer   src_input;
  MemoryBuffer   src_output;
protected:
  FXbool processing;
protected:
  FXint     stream;
  FXint     stream_length;
  FXint     stream_remaining;
  FXint     stream_written;
  FXint     stream_position;
  FXint     timestamp;
protected:
  void configure(const AudioFormat&);
  void load_plugin();
  void unload_plugin();
  void close_plugin();
  void process(Packet*);

#ifdef HAVE_SAMPLERATE_PLUGIN
  void resample(Packet*,FXint & nframes);
#endif

  void drain(FXbool flush=true);

  void update_position(FXint stream,FXint position,FXint nframes,FXint length);
  void notify_position();
  void reset_position();
public:
  OutputThread(AudioEngine*);
  virtual FXint process(Event*);
  virtual FXint run();
  virtual ~OutputThread();
public:
  FXString getOutputPlugin() const;
  };

#endif


