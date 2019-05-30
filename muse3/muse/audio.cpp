//=========================================================
//  MusE
//  Linux Music Editor
//  $Id: audio.cpp,v 1.59.2.30 2009/12/20 05:00:35 terminator356 Exp $
//
//  (C) Copyright 2001-2004 Werner Schweer (ws@seh.de)
//  (C) Copyright 2011 Tim E. Real (terminator356 on users dot sourceforge dot net)
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//=========================================================

#include "muse_math.h"
#include <set>
#include <errno.h>
#include <fcntl.h>

#include "app.h"
#include "song.h"
#include "node.h"
#include "audiodev.h"
#include "mididev.h"
#include "midiport.h"
#include "alsamidi.h"
#include "synth.h"
#include "audioprefetch.h"
#include "plugin.h"
#include "audio.h"
#include "wave.h"
#include "midictrl.h"
#include "midiseq.h"
#include "sync.h"
#include "midi.h"
#include "event.h"
#include "gconfig.h"
#include "pos.h"
#include "ticksynth.h"
//#include "operations.h"
#include "undo.h"
#include "globals.h"
#include "large_int.h"

#ifdef _WIN32
#define pipe(fds) _pipe(fds, 4096, _O_BINARY)
#endif

// Experimental for now - allow other Jack timebase masters to control our midi engine.
// TODO: Be friendly to other apps and ask them to be kind to us by using jack_transport_reposition. 
//       It is actually required IF we want the extra position info to show up
//        in the sync callback, otherwise we get just the frame only.
//       This information is shared on the server, it is directly passed around. 
//       jack_transport_locate blanks the info from sync until the timebase callback reads 
//        it again right after, from some timebase master. 
//       Sadly not many of us use jack_transport_reposition. So we need to work around it !
//#define _JACK_TIMEBASE_DRIVES_MIDI_

#ifdef _JACK_TIMEBASE_DRIVES_MIDI_
#include "jackaudio.h"  
#endif

// Undefine if and when multiple output routes are added to midi tracks.
#define _USE_MIDI_TRACK_SINGLE_OUT_PORT_CHAN_

// For debugging metronome and precount output: Uncomment the fprintf section.
#define DEBUG_MIDI_METRONOME(dev, format, args...) // fprintf(dev, format, ##args);
// For debugging midi timing: Uncomment the fprintf section.
#define DEBUG_TRANSPORT_SYNC(dev, format, args...) // fprintf(dev, format, ##args);

namespace MusEGlobal {
MusECore::Audio* audio = NULL;
MusECore::AudioDevice* audioDevice = NULL;   // current audio device in use
extern unsigned int volatile midiExtSyncTicks;   
}

namespace MusECore {

void initAudio()   
{
      MusEGlobal::audio = new Audio();
}

extern uint64_t curTimeUS();

const char* seqMsgList[] = {
      "SEQM_REVERT_OPERATION_GROUP", "SEQM_EXECUTE_OPERATION_GROUP",
      "SEQM_EXECUTE_PENDING_OPERATIONS", 
      "SEQM_RESET_DEVICES", "SEQM_INIT_DEVICES", "SEQM_PANIC",
      "SEQM_MIDI_LOCAL_OFF",
      "SEQM_PLAY_MIDI_EVENT",
      "SEQM_SET_HW_CTRL_STATE",
      "SEQM_SET_HW_CTRL_STATES",
      "SEQM_SET_TRACK_AUTO_TYPE",
      "SEQM_SET_AUX",
      "SEQM_UPDATE_SOLO_STATES",
      "AUDIO_ROUTEADD", "AUDIO_ROUTEREMOVE", "AUDIO_REMOVEROUTES",
      "AUDIO_ADDPLUGIN",
      "AUDIO_SET_PREFADER", "AUDIO_SET_CHANNELS",
      "AUDIO_SWAP_CONTROLLER_IDX",
      "AUDIO_CLEAR_CONTROLLER_EVENTS",
      "AUDIO_SEEK_PREV_AC_EVENT",
      "AUDIO_SEEK_NEXT_AC_EVENT",
      "AUDIO_ERASE_AC_EVENT",
      "AUDIO_ERASE_RANGE_AC_EVENTS",
      "AUDIO_ADD_AC_EVENT",
      "AUDIO_CHANGE_AC_EVENT",
      "AUDIO_SET_SEND_METRONOME", 
      "AUDIO_START_MIDI_LEARN",
      "MS_PROCESS", "MS_STOP", "MS_SET_RTC", "MS_UPDATE_POLL_FD",
      "SEQM_IDLE", "SEQM_SEEK",
      "AUDIO_WAIT"
      };

const char* audioStates[] = {
      "STOP", "START_PLAY", "PLAY", "LOOP1", "LOOP2", "SYNC", "PRECOUNT"
      };


//---------------------------------------------------------
//   Audio
//---------------------------------------------------------

const int Audio::_extClockHistoryCapacity = 8192;
      
Audio::Audio()
      {
      _running      = false;
      recording     = false;
      idle          = false;
      _freewheel    = false;
      _bounce       = false;
      _loopFrame    = 0;
      _loopCount    = 0;
      m_Xruns       = 0;

      _pos.setType(Pos::FRAMES);
      _pos.setFrame(0);
#ifdef _AUDIO_USE_TRUE_FRAME_
      _previousPos.setType(Pos::FRAMES);
      _previousPos.setFrame(0);
#endif
      _curCycleFrames = 0;
      nextTickPos = curTickPos = 0;
      _precountFramePos = 0;
      framesBeat = 0;
      framesBeatDivisor = 0;
      framesBeatRemainder = 0;
      precountMidiClickFrame = 0;
      precountMidiClickFrameRemainder = 0;
      precountTotalFrames = 0;
      _syncPlayStarting = false;
      // Set for way beyond the end of the expected count.
      _antiSeekFloodCounter = 100000.0;
      
      midiClick     = 0;
      audioClick    = 0;
      clickno       = 0;
      clicksMeasure = 0;
      _extClockHistory = new ExtMidiClock[_extClockHistoryCapacity];
      _extClockHistorySize = 0;

      syncTimeUS    = 0;
      syncFrame     = 0;

      state         = STOP;
      msg           = 0;

      startRecordPos.setType(Pos::FRAMES);  // Tim
      endRecordPos.setType(Pos::FRAMES);
      startExternalRecTick = 0;
      endExternalRecTick = 0;
      
      //---------------------------------------------------
      //  establish pipes/sockets
      //---------------------------------------------------

      int filedes[2];         // 0 - reading   1 - writing
      if (pipe(filedes) == -1) {
            perror("creating pipe0");
            exit(-1);
            }
      fromThreadFdw = filedes[1];
      fromThreadFdr = filedes[0];
#ifndef _WIN32
      int rv = fcntl(fromThreadFdw, F_SETFL, O_NONBLOCK);
      if (rv == -1)
            perror("set pipe O_NONBLOCK");
#endif
      if (pipe(filedes) == -1) {
            perror("creating pipe1");
            exit(-1);
            }
      sigFd = filedes[1];
      sigFdr = filedes[0];
      }

Audio::~Audio() 
{
  if(_extClockHistory)
    delete[] _extClockHistory;
} 

//---------------------------------------------------------
//   start
//    start audio processing
//---------------------------------------------------------

extern bool initJackAudio();

bool Audio::start()
      {
      state = STOP;
      _loopCount = 0;
      
      //MusEGlobal::muse->setHeartBeat();  // Moved below
      
      if (!MusEGlobal::audioDevice) {
          if(initJackAudio() == false) {
                InputList* itl = MusEGlobal::song->inputs();
                for (iAudioInput i = itl->begin(); i != itl->end(); ++i) {
                      if (MusEGlobal::debugMsg) fprintf(stderr, "reconnecting input %s\n", (*i)->name().toLatin1().data());
                      for (int x=0; x < (*i)->channels();x++)
                          (*i)->setJackPort(x,0);
                      (*i)->setName((*i)->name()); // restore jack connection
                      }

                OutputList* otl = MusEGlobal::song->outputs();
                for (iAudioOutput i = otl->begin(); i != otl->end(); ++i) {
                      if (MusEGlobal::debugMsg) fprintf(stderr, "reconnecting output %s\n", (*i)->name().toLatin1().data());
                      for (int x=0; x < (*i)->channels();x++)
                          (*i)->setJackPort(x,0);
                      if (MusEGlobal::debugMsg) fprintf(stderr, "name=%s\n",(*i)->name().toLatin1().data());
                      (*i)->setName((*i)->name()); // restore jack connection
                      }
               }
          else {
               fprintf(stderr, "Failed to init audio!\n");
               return false;
               }
          }

      _running = true;  // Set before we start to avoid error messages in process.
      if(!MusEGlobal::audioDevice->start(MusEGlobal::realTimePriority))
      {
        fprintf(stderr, "Failed to start audio!\n");
        _running = false;
        return false;
      }

      // shall we really stop JACK transport and locate to
      // saved position?

      MusEGlobal::audioDevice->stopTransport();  
      
      MusEGlobal::audioDevice->seekTransport(MusEGlobal::song->cPos());   
      
      // Should be OK to start this 'leisurely' timer only after everything
      //  else has been started.
      MusEGlobal::muse->setHeartBeat();
      
      return true;
      }

//---------------------------------------------------------
//   stop
//    stop audio processing
//---------------------------------------------------------

void Audio::stop(bool)
      {
      // Stop timer. Possible random crashes closing a song and loading another song - 
      //  observed a few times in mixer strip timer handlers (updatexxx). Not sure how
      //  (we're in the graphics thread), but in case something during loading runs
      //  the event loop or something, this should at least not hurt. 2019/01/24 Tim.
      MusEGlobal::muse->stopHeartBeat();

      if (MusEGlobal::audioDevice)
            MusEGlobal::audioDevice->stop();
      _running = false;
      }

//---------------------------------------------------------
//   sync
//    return true if sync is completed
//---------------------------------------------------------

bool Audio::sync(int jackState, unsigned frame)
{
  DEBUG_TRANSPORT_SYNC(stderr, "Audio::sync() begin: state:%d jackState:%d frame:%u pos frame:%u\n", state, jackState, frame, _pos.frame());
    
  bool done = true;
  if (state == PRECOUNT)
  {
      DEBUG_TRANSPORT_SYNC(stderr, "Audio::sync: PRECOUNT: _precountFramePos:%u precountTotalFrames:%u\n", 
      _precountFramePos, precountTotalFrames);
    
    // Is it requesting a different frame (seek during sync)?
    // Don't interrupt freewheel mode.
    if (frame != _pos.frame() && !_freewheel)
    {
      DEBUG_TRANSPORT_SYNC(stderr, "   state == PRECOUNT, pos frame:%u req frame:%u calling seek...\n", _pos.frame(), frame);
      seek(Pos(frame, false));
      
      done = MusEGlobal::audioPrefetch->seekDone();
    
      // We're in precount state, so it must have started from stop state. Set to true.
      _syncPlayStarting = true;
    
      // Is the audio prefetch already done?
      if(done)
      {
        // Reset the flag now.
        _syncPlayStarting = false;
        // Does the precount start over again? Return false if so, to continue holding up the transport.
        // This will start the precount if necessary and set the state to PRECOUNT.
        if(startPreCount())
          return false;
      }
      
      state = START_PLAY;
      return done;
    }
    
    return _precountFramePos >= precountTotalFrames;
  }
          
  if (state == LOOP1)
  {
    state = LOOP2;
  }
  else
  {
    State s = State(jackState);
    
    //  STOP -> START_PLAY      start rolling
    //  STOP -> STOP            seek in stop state
    //  PLAY -> START_PLAY  seek in play state

    if (state != START_PLAY)
    {
      Pos p(frame, false);
      DEBUG_TRANSPORT_SYNC(stderr, "   state != START_PLAY, req frame:%u calling seek...\n", frame);
      seek(p);
      if (!_freewheel)
        done = MusEGlobal::audioPrefetch->seekDone();
      
      if (s == START_PLAY)
      {
        _syncPlayStarting = (state == STOP);
        
        DEBUG_TRANSPORT_SYNC(stderr, "state != START_PLAY: done:%d\n", done);
        
        // Is this a STOP to START_PLAY transition?
        if(_syncPlayStarting)
        {
          // Stop the timer.
          _antiSeekFloodCounter = 100000.0;

          // Is the audio prefetch already done?
          if(done)
          {
            // Reset the flag now.
            _syncPlayStarting = false;
            // Does the precount start? Return false if so, to begin holding up the transport.
            // This will start the precount if necessary and set the state to PRECOUNT.
            if(startPreCount())
              return false;
          }
        }
        else
        {
          // Start the timer.
          _antiSeekFloodCounter = 0.0;
        }
          
        state = START_PLAY;
      }
    }
    else
    {
      // Is it requesting a different frame (seek during seek)?
      // Don't interrupt freewheel mode.
      if (frame != _pos.frame() && !_freewheel)
      {
        DEBUG_TRANSPORT_SYNC(stderr, "   state == START_PLAY, pos frame:%u req frame:%u calling seek...\n", _pos.frame(), frame);
        seek(Pos(frame, false));
        
        // Reset timer to start it.
        _antiSeekFloodCounter = 0.0;
      }
      
      done = MusEGlobal::audioPrefetch->seekDone();
      
      if(_antiSeekFloodCounter < 0.4)
      {
        done = false;
        _antiSeekFloodCounter += (float)MusEGlobal::segmentSize / (float)MusEGlobal::sampleRate;
      }
      
      DEBUG_TRANSPORT_SYNC(stderr, "state == START_PLAY: done:%d\n", done);
      
      // Was this a STOP to START_PLAY transition?
      if(_syncPlayStarting)
      {
        // Are the audio prefetch done and anti-flood timer done?
        if(done)
        {
          // Reset the flag now.
          _syncPlayStarting = false;
          // Does the precount start? Return false if so, to continue (or begin) holding up the transport.
          // This will start the precount if necessary and set the state to PRECOUNT.
          if(startPreCount())
            return false;
        }
      }
    }
  }
  
  //fprintf(stderr, "Audio::sync() end: state:%d pos frame:%u\n", state, _pos.frame());
  return done;
}

//---------------------------------------------------------
//   startPreCount
//    During sync(), starts count-in state if necessary and returns whether 
//     count-in state has been started.
//---------------------------------------------------------

bool Audio::startPreCount()
{
  MusECore::MetronomeSettings* metro_settings = 
    MusEGlobal::metroUseSongSettings ? &MusEGlobal::metroSongSettings : &MusEGlobal::metroGlobalSettings;

  // Since other Jack clients might also set the sync timeout at any time,
  //  we need to be constantly enforcing our desired limit!
  // Since setSyncTimeout() may not be realtime friendly (Jack driver),
  //  we set the driver's sync timeout in the gui thread.
  // Sadly, we likely cannot get away with setting it here via the audio sync callback.
  // So whenever stop, start or seek occurs, we'll try to casually enforce the timeout in Song::seqSignal().
  // It's casual, unfortunately we can't set the EXACT timeout amount here when we really need to.
  
  if (metro_settings->precountEnableFlag
    && MusEGlobal::song->click()
    && !MusEGlobal::extSyncFlag.value()
    && ((!MusEGlobal::song->record() && metro_settings->precountOnPlay) || MusEGlobal::song->record()))
  {
        DEBUG_MIDI_METRONOME(stderr, "state = PRECOUNT!\n");
        state = PRECOUNT;
        
        int bar, beat;
        unsigned tick;
        MusEGlobal::sigmap.tickValues(curTickPos, &bar, &beat, &tick);

        int z, n;
        if (metro_settings->precountFromMastertrackFlag)
              MusEGlobal::sigmap.timesig(curTickPos, z, n);
        else {
              z = metro_settings->precountSigZ;
              n = metro_settings->precountSigN;
              }
        clickno       = 0;
        int clicks_total = z * metro_settings->preMeasures;
        clicksMeasure = z;
        int ticks_beat     = (MusEGlobal::config.division * 4)/n;
        // The number of frames per beat in precount state.
//         framesBeat    = MusEGlobal::tempomap.ticks2frames((MusEGlobal::config.division * 4) / n, curTickPos);
        
        
        
        // The number of frames per beat in precount state.
        // Tick resolution is less than frame resolution. 
        // Round up so that the reciprocal function (frame to tick) matches value for value.
        //framesBeat = muse_multiply_64_div_64_to_64(
        //  (uint64_t)MusEGlobal::sampleRate * (uint64_t)MusEGlobal::tempomap.tempo(curTickPos), (MusEGlobal::config.division * 4) / n,
        //  (uint64_t)MusEGlobal::config.division * (uint64_t)MusEGlobal::tempomap.globalTempo() * 10000UL, true);
        //framesBeat = 
        //  (4UL * (uint64_t)MusEGlobal::sampleRate * (uint64_t)MusEGlobal::tempomap.tempo(curTickPos)) /
        //  ((uint64_t)n * (uint64_t)MusEGlobal::tempomap.globalTempo() * 10000UL);
        const uint64_t framesBeatDividend = (uint64_t)MusEGlobal::sampleRate * (uint64_t)MusEGlobal::tempomap.tempo(curTickPos) * 4;
        framesBeatDivisor = n * MusEGlobal::tempomap.globalTempo() * 10000;
        framesBeat = framesBeatDividend / framesBeatDivisor;
        framesBeatRemainder = framesBeatDividend % framesBeatDivisor;
        
        // Reset to zero.
        _precountFramePos = 0;
         
        // How many ticks are required for the precount?
        const unsigned int precount_ticks = clicks_total * ticks_beat;
        // How many extra precount ticks are required to 'fill in' for a mid-bar curTickPos?
        const unsigned int extra_precount_ticks = beat * ticks_beat + tick;
        // What is the current tick's frame (not the more accurate device frame)?
        const unsigned int cur_tick_frame = Pos(curTickPos, true).frame();
        // How many total frames are required for the precount?
        precountTotalFrames = 
          MusEGlobal::tempomap.ticks2frames(precount_ticks + extra_precount_ticks, curTickPos);
        // How many more (or less) frames are required, in case the cursor is on a frame in-between ticks?
        // If lrint() rounding is used in our tempo routines, the difference between current tick's frame 
        //  and current device frame can actually be negative!
        if(_pos.frame() > cur_tick_frame)
        {
          precountTotalFrames += (_pos.frame() - cur_tick_frame);
        }
        else
        {
          // Basic protection from underflow.
          if(precountTotalFrames < (cur_tick_frame - _pos.frame()))
            precountTotalFrames = 0;
          else
            precountTotalFrames -= cur_tick_frame - _pos.frame();
        }
        
        // What is the remainder of the total frames divided by the segment size?
        // This is the value that the precount frame times should be shifted backwards so that
        //  the very last precount frame + 1 lines up exactly with the first metronome frame.
        // But since we cannot schedule before the current time, we trick it by shifting
        //  forward and sacrificing one cycle. Therefore this value is referenced to the 
        //  start of a cycle buffer, not the end.
        // The offset ranges from 1 to segmentSize inclusive.
        unsigned int precount_frame_offset = MusEGlobal::segmentSize - (precountTotalFrames % MusEGlobal::segmentSize);
        // Start the precount at this frame.
        precountMidiClickFrame = precount_frame_offset;
        precountMidiClickFrameRemainder = 0;
        // Make room in the total frames for the offset as well.
        precountTotalFrames += precount_frame_offset;

        DEBUG_MIDI_METRONOME(stderr, "Audio::startPreCount: _pos.tick():%u _pos.frame():%u "
          "curTickPos:%u cur_tick_frame:%u extra_precount_frames:%d "
          "clicks_total:%d framesBeat:%u precount_ticks:%u extra_precount_ticks:%u\n"
          "precountTotalFrames:%u precount_frame_offset:%u precountMidiClickFrame:%u\n",
          _pos.tick(), _pos.frame(), curTickPos, cur_tick_frame, _pos.frame() - cur_tick_frame, clicks_total, framesBeat, 
          precount_ticks, extra_precount_ticks, precountTotalFrames, precount_frame_offset, precountMidiClickFrame);
      
        return true;
  }
  
  return false;
}

//---------------------------------------------------------
//   reSyncAudio
//    To be called from audio thread only.
//---------------------------------------------------------

void Audio::reSyncAudio()
{
  if (isPlaying()) 
  {
    if (!MusEGlobal::checkAudioDevice()) return;
#ifdef _AUDIO_USE_TRUE_FRAME_
    _previousPos = _pos;
#endif
    // NOTE: Comment added by Tim: This line is crucial if the tempo is changed during playback,
    //  either via changes to tempo map or the static tempo value. The actual transport frame is allowed
    //  to continue progressing naturally but our representation of it (_pos) jumps to a new value
    //  so that the relation between ticks and frames in all our tempo routines remains correct.
    // But the relation betweeen actual transport frame and current tick will still be incorrect
    //  for the given new tempo value, that is why this adjustment to our _pos is needed.
    // Another solution would be to actually seek the transport to the correct frame.
    // In either solution any waves will jump to a new position but midi will continue on.
    // A third solution is not jump at all. Instead curTickPos is incremented with a 'delta' rather
    //  than by incrementing _pos and converting to tick. Both midi and audio progress naturally.
    // But this has a problem in that we cannot use fractional remainder techniques because the
    //  tempo is part of the denominator and changes with every tempo change, so adding incremental
    //  fractions with different denominators together would be tough (cross multiply etc.).
    // Seems the only way for a delta then is to express it as say microticks (1/1,000,000 of a tick).
    // But even microticks may not be enough, calculations show nanoticks would be better.
    // The issue is that the resolution must be very high so that the slight error in the
    //  increment value does not build up over time and cause drift between frame and tick.
    _pos.setTick(curTickPos);
    syncFrame     = MusEGlobal::audioDevice->framesAtCycleStart();
    syncTimeUS    = curTimeUS();
  }
}  
      
//---------------------------------------------------------
//   setFreewheel
//---------------------------------------------------------

void Audio::setFreewheel(bool val)
      {
      _freewheel = val;
      }

//---------------------------------------------------------
//   shutdown
//---------------------------------------------------------

void Audio::shutdown()
      {
      _running = false;
      fprintf(stderr, "Audio::shutdown()\n");
      write(sigFd, "S", 1);
      }

//---------------------------------------------------------
//   process
//    process one audio buffer at position "_pos "
//    of size "frames"
//---------------------------------------------------------

void Audio::process(unsigned frames)
      {
      _curCycleFrames = frames;
      if (!MusEGlobal::checkAudioDevice()) return;
      if (msg) {
            processMsg(msg);
            int sn = msg->serialNo;
            msg    = 0;    // don't process again
            int rv = write(fromThreadFdw, &sn, sizeof(int));
            if (rv != sizeof(int)) {
                  fprintf(stderr, "audio: write(%d) pipe failed: %s\n",
                     fromThreadFdw, strerror(errno));
                  }
            }

      OutputList* ol = MusEGlobal::song->outputs();
      if (idle) {
            // deliver no audio
            for (iAudioOutput i = ol->begin(); i != ol->end(); ++i)
                  (*i)->silence(frames);
            return;
            }

      //
      // resync with audio interface
      //
      syncFrame   = MusEGlobal::audioDevice->framesAtCycleStart();
      syncTimeUS  = curTimeUS();
      
      int jackState = MusEGlobal::audioDevice->getState();

      //DEBUG_MIDI_TIMING(stderr, "Audio::process Current state:%s jackState:%s sync frame:%u pos frame:%u current transport frame:%u\n", 
      //        audioStates[state], audioStates[jackState], syncFrame, _pos.frame(), MusEGlobal::audioDevice->curTransportFrame());
      
      if ((state == START_PLAY || state == PRECOUNT) && jackState == PLAY) {
            _loopCount = 0;
            MusEGlobal::song->reenableTouchedControllers();
            startRolling();
            if (_bounce)
                  write(sigFd, "f", 1);
            }
      else if (state == LOOP2 && jackState == PLAY) {
            ++_loopCount;                  // Number of times we have looped so far
            Pos newPos(_loopFrame, false);
            seek(newPos);
            startRolling();
            }
      else if ((isPlaying() || state == PRECOUNT) && jackState == STOP) {
            stopRolling();
            }
      else if (state == START_PLAY && jackState == STOP) {
            state = STOP;
            if (_bounce) {
                  MusEGlobal::audioDevice->startTransport();
                  }
            else
                  write(sigFd, "3", 1);   // abort rolling
            }
      else if (state == STOP && jackState == PLAY) {
            _loopCount = 0;
            MusEGlobal::song->reenableTouchedControllers();
            startRolling();
            }
      else if (state == LOOP1 && jackState == PLAY)
            ;     // treat as play
      else if (state == LOOP2 && jackState == START_PLAY) {
            ;     // sync cycle
            }
      else if (state == PRECOUNT && jackState == START_PLAY) {
            ;     // sync cycle
            }
      else if (state != jackState)
            fprintf(stderr, "JACK: state transition %s -> %s ?\n",
               audioStates[state], audioStates[jackState]);

      // fprintf(stderr, "p %s %s %d\n", audioStates[jackState], audioStates[state], _pos.frame());

      //
      // clear aux send buffers
      //
      AuxList* al = MusEGlobal::song->auxs();
      for (unsigned i = 0; i < al->size(); ++i) {
            AudioAux* a = (AudioAux*)((*al)[i]);
            float** dst = a->sendBuffer();
            for (int ch = 0; ch < a->channels(); ++ch)
                  memset(dst[ch], 0, sizeof(float) * MusEGlobal::segmentSize);
            }

      for (iAudioOutput i = ol->begin(); i != ol->end(); ++i)
            (*i)->processInit(frames);
      int samplePos = _pos.frame();
      int offset    = 0;      // buffer offset in audio buffers
#ifdef _JACK_TIMEBASE_DRIVES_MIDI_              
      bool use_jack_timebase = false;
#endif

      for(iMidiDevice id = MusEGlobal::midiDevices.begin(); id != MusEGlobal::midiDevices.end(); ++id)
      {
        MidiDevice* md = (*id);
        const int port = md->midiPort();
        
        // Gather midi input from devices that need collecting, such as Jack midi.
        md->collectMidiEvents();
        
        // Process the selected device's external clock history fifo ring buffer.
        // Quickly transfer the items to a list for easier processing later.
        // It is possible for the timestamps to be out of order. Deal with it.
        // Sort all the timestamps. Do not miss a clock, better that it is at least
        //  included in the count.
        if(port >= 0 && port < MusECore::MIDI_PORTS && port == MusEGlobal::config.curMidiSyncInPort)
        {
          // False = don't use the size snapshot.
          const int clk_fifo_sz = md->extClockHistory()->getSize(false);
          if(clk_fifo_sz != 0)
          {
            for(int i = 0; i < clk_fifo_sz; ++i)
            {
              if(_extClockHistorySize >= _extClockHistoryCapacity)
              {
                fprintf(stderr, "Audio::process: _extClockHistory overrun!\n");
                break;
              }
              _extClockHistory[_extClockHistorySize] = md->extClockHistory()->get();
              ++_extClockHistorySize;
            }
          }
        }
        else
          // Otherwise flush and discard the device's unused ring buffer data.
          md->extClockHistory()->clearRead();
      }
      
      //if(MusEGlobal::extSyncFlag.value() && (MusEGlobal::midiSyncContainer.isRunning() || isPlaying()))
      //  fprintf(stderr, "extSyncFlag:%d  externalPlayState:%d isPlaying:%d\n",
      //    MusEGlobal::extSyncFlag.value(), MusEGlobal::midiSyncContainer.externalPlayState(), isPlaying());
      
      if (isPlaying()) {
            if (!freewheel())
                  MusEGlobal::audioPrefetch->msgTick(isRecording(), true);

            if (_bounce && _pos >= MusEGlobal::song->rPos()) {
                  _bounce = false;
                  write(sigFd, "F", 1);
                  return;
                  }
                  
#ifdef _JACK_TIMEBASE_DRIVES_MIDI_
            unsigned curr_jt_tick, next_jt_ticks;
            use_jack_timebase = 
                MusEGlobal::audioDevice->deviceType() == AudioDevice::JACK_AUDIO && 
                !MusEGlobal::jackTransportMaster && 
                !MusEGlobal::song->masterFlag() &&
                !MusEGlobal::extSyncFlag.value() &&
                static_cast<MusECore::JackAudioDevice*>(MusEGlobal::audioDevice)->timebaseQuery(
                  frames, NULL, NULL, NULL, &curr_jt_tick, &next_jt_ticks);
            // NOTE: I would rather trust the reported current tick than rely solely on the stream of 
            // tempos to correctly advance to the next position (which did actually test OK anyway).
            if(use_jack_timebase)
              curTickPos = curr_jt_tick;
#endif
            
            //
            //  check for end of song
            //
            if ((curTickPos >= MusEGlobal::song->len())
               && !(MusEGlobal::song->record()
                || _bounce
                || MusEGlobal::song->loop())) {

                  if(MusEGlobal::debugMsg)
                    fprintf(stderr, "Audio::process curTickPos >= MusEGlobal::song->len\n");
                  
                  MusEGlobal::audioDevice->stopTransport();
                  return;
                  }

            //
            //  check for loop end
            //
            if (state == PLAY && MusEGlobal::song->loop() && !_bounce && !MusEGlobal::extSyncFlag.value()) {
                  const Pos& loop = MusEGlobal::song->rPos();
                  unsigned n = loop.frame() - samplePos - (3 * frames);
                  if (n < frames) {
                        // loop end in current cycle
                        unsigned lpos = MusEGlobal::song->lPos().frame();
                        // adjust loop start so we get exact loop len
                        if (n > lpos)
                              n = 0;
                        state = LOOP1;
                        _loopFrame = lpos - n;
                        Pos lp(_loopFrame, false);
                        // Seek the transport. Note that temporary clearing of sustain
                        //  controllers is done by the seek handlers and then startRolling().
                        MusEGlobal::audioDevice->seekTransport(lp);
                        }
                  }
            
            if(MusEGlobal::extSyncFlag.value())        // p3.3.25
            {
              // Advance the tick position by the number of clock events times the division.
              const int div = MusEGlobal::config.division / 24;
              int tcks = 0;
              for(int i = 0; i < _extClockHistorySize; ++i)
              {
                if(_extClockHistory[i].isPlaying())
                  ++tcks;
              }
              nextTickPos = curTickPos + tcks * div;
            }
            else
            {

#ifdef _JACK_TIMEBASE_DRIVES_MIDI_              
              if(use_jack_timebase)
                // With jack timebase this might not be accurate -
                //  we are relying on the tempo to figure out the next tick.
                nextTickPos = curTickPos + next_jt_ticks;
              else
#endif                
              {
                Pos ppp(_pos);
                ppp += frames;
                nextTickPos = ppp.tick();
              }
            }
          }

      process1(samplePos, offset, frames);
      for (iAudioOutput i = ol->begin(); i != ol->end(); ++i)
            (*i)->processWrite();
      
#ifdef _AUDIO_USE_TRUE_FRAME_
      _previousPos = _pos;
#endif
// REMOVE Tim. latency. Changed. Hm, doesn't work. Position takes a long time to start moving.
      if (isPlaying()) {
      //if(isPlaying() || (MusEGlobal::extSyncFlag.value() && MusEGlobal::midiSyncContainer.isPlaying())) {
            _pos += frames;
            // With jack timebase this might not be accurate if we 
            //  set curTickPos (above) from the reported current tick.
            curTickPos = nextTickPos; 
            }
      
      // If external sync has started but the transport has not started yet,
      //  don't reset the clock history yet, just let it pile up until the transport starts.
      // It's because curTickPos does not advance yet until transport is running, so we
      //  can't rely on curTickPos as a base just yet...
      if(!MusEGlobal::extSyncFlag.value() || !MusEGlobal::midiSyncContainer.isPlaying() || isPlaying())
        _extClockHistorySize = 0;
      }

//---------------------------------------------------------
//   process1
//---------------------------------------------------------

void Audio::process1(unsigned samplePos, unsigned offset, unsigned frames)
      {
// REMOVE Tim. latency. Removed. Moved below.
//       processMidi(frames);

      //
      // process not connected tracks
      // to animate meter display
      //
      TrackList* tl = MusEGlobal::song->tracks();
      MidiDeviceList& mdl = MusEGlobal::midiDevices;
      Track* track; 
      AudioTrack* atrack; 
      int channels;
      bool want_record_side;
      // REMOVE Tim. latency. Changed.
//       for(ciTrack it = tl->begin(); it != tl->end(); ++it) 
//       {
//         if((*it)->isMidiTrack())
//           continue;
//         track = (AudioTrack*)(*it);
//         
//         // For audio track types, synths etc. which need some kind of non-audio 
//         //  (but possibly audio-affecting) processing always, even if their output path
//         //  is ultimately unconnected.
//         // Example: A fluidsynth instance whose output path ultimately led to nowhere 
//         //  would not allow us to load a font. Since process() was driven by audio output,
//         //  in this case there was nothing driving the process() function which responds to
//         //  such gui commands. So I separated the events processing from process(), into this.
//         // It should be used for things like midi events, gui events etc. - things which need to
//         //  be done BEFORE all the AudioOutput::process() are called below. That does NOT include 
//         //  audio processing, because THAT is done at the very end of this routine.
//         // This will also reset the track's processed flag.         Tim.
//         track->preProcessAlways();
//       }

      MusECore::MetronomeSettings* metro_settings = 
        MusEGlobal::metroUseSongSettings ? &MusEGlobal::metroSongSettings : &MusEGlobal::metroGlobalSettings;

      // This includes synthesizers.
      for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it) 
      {
        track = *it;
        
        // For track types, synths etc. which need some kind of non-audio 
        //  (but possibly audio-affecting) processing always, even if their output path
        //  is ultimately unconnected.
        // Example: A fluidsynth instance whose output path ultimately led to nowhere 
        //  would not allow us to load a font. Since process() was driven by audio output,
        //  in this case there was nothing driving the process() function which responds to
        //  such gui commands. So I separated the events processing from process(), into this.
        // It should be used for things like midi events, gui events etc. - things which need to
        //  be done BEFORE all the AudioOutput::process() are called below. That does NOT include 
        //  audio processing, because THAT is done at the very end of this routine.
        // This will also reset the track's processed flag.         Tim.
        track->preProcessAlways();

        // Reset some latency info to prepare for (re)computation.
        // TODO: Instead of doing this blindly every cycle, do it only when
        //        a latency controller changes or a connection is made etc,
        //        ie only when something changes.
        track->prepareLatencyScan();
      }

      // This includes synthesizers.
      for(ciMidiDevice imd = mdl.cbegin(); imd != mdl.cend(); ++imd) 
      {
        MidiDevice* md = *imd;
        // Device not in use?
        if(md->midiPort() < 0 || md->midiPort() >= MusECore::MIDI_PORTS)
          continue;

        // Reset some latency info to prepare for (re)computation.
        // TODO: Instead of doing this blindly every cycle, do it only when
        //        a latency controller changes or a connection is made etc,
        //        ie only when something changes.
        md->prepareLatencyScan();
      }
      
      // Pre-process the metronome.
      // REMOVE Tim. latency. Changed.
//       ((AudioTrack*)metronome)->preProcessAlways();
      metronome->preProcessAlways();
      // Reset some latency info to prepare for (re)computation.
      // TODO: Instead of doing this blindly every cycle, do it only when
      //        a latency controller changes or a connection is made etc,
      //        ie only when something changes.
      static_cast<AudioTrack*>(metronome)->prepareLatencyScan();
      static_cast<MidiDevice*>(metronome)->prepareLatencyScan();




      // REMOVE Tim. latency. Added.
      //---------------------------------------------
      // Latency correction/compensation processing
      //---------------------------------------------

      if(MusEGlobal::config.enableLatencyCorrection)
      {
        float song_worst_latency = 0.0f;
        
        //---------------------------------------------
        // PASS 1: Find any dominant branches:
        //---------------------------------------------

        for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it) 
        {
          track = *it;
          
          // If the track is for example a Wave Track, we must consider up to two contributing paths,
          //  the output (playback) side and the input (record) side which can pass through via monitoring.
          want_record_side = track->isMidiTrack() || track->type() == Track::WAVE;
          
          //---------------------------------------------
          // We are looking for the end points of branches.
          // This includes Audio Outputs, and open-ended branches
          //  that go nowhere or are effectively disconnected from
          //  their destination(s).
          // This caches its result in the track's _latencyInfo structure
          //  so it can be used faster in the correction pass or final pass.
          //---------------------------------------------
          
          // Examine any recording path, if desired.
          if(want_record_side && track->isLatencyInputTerminal())
            track->getDominanceInfo(true);
          
          // Examine any playback path.
          if(track->isLatencyOutputTerminal())
            track->getDominanceInfo(false);
        }      

  #if 1
        // This includes synthesizers.
        for(ciMidiDevice imd = mdl.cbegin(); imd != mdl.cend(); ++imd) 
        {
          MidiDevice* md = *imd;
          // Device not in use?
          if(md->midiPort() < 0 || md->midiPort() >= MusECore::MIDI_PORTS)
            continue;

          // Examine any playback path on capture devices.
          if(md->isLatencyOutputTerminalMidi(true /*capture*/))
            md->getDominanceInfoMidi(true /*capture*/, false);

          // Examine any playback path on playback devices.
          if(md->isLatencyOutputTerminalMidi(false /*playback*/))
            md->getDominanceInfoMidi(false /*playback*/, false);
        }
  #endif      

        //---------------------------------------------
        // Throughout this latency code, we will examine the metronome
        //  enable/disable settings rather than the metronome on/off button(s),
        //  so that the metronome button(s) can be 'ready for action' when clicked,
        //  without affecting the sound.
        // Otherwise a glitch would occur every time the metronome is simply
        //  turned on or off via the gui toolbars, while latency is recomputed.
        //---------------------------------------------

        //if(MusEGlobal::song->click())
        if(metro_settings->audioClickFlag)
        {
          // Examine any playback path.
          if(metronome->isLatencyOutputTerminal())
            metronome->getDominanceInfo(false);
        }
        if(metro_settings->midiClickFlag)
        {
          // Examine any playback path.
          if(metronome->isLatencyOutputTerminalMidi(false /*playback*/))
            metronome->getDominanceInfoMidi(false /*playback*/, false);
        }


        //---------------------------------------------
        // PASS 2: Set correction values:
        //---------------------------------------------

        // Now that we know the worst case latency,
        //  set all the branch correction values.
        for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it) 
        {
          track = *it;
          
          // If the track is for example a Wave Track, we must consider up to two contributing paths,
          //  the output (playback) side and the input (record) side which can pass through via monitoring.
          want_record_side = track->isMidiTrack() || track->type() == Track::WAVE;

          // Set branch correction values, for any tracks which support it.
          // If this branch can dominate latency, ie. is fed from an Audio Input Track,
          //  then we cannot apply correction to the branch.
          // For example:
          //
          //  Input(512) -> Wave1(monitor on) -> Output(3072)
          //                                  -> Wave2
          //
          // Wave1 wants to set a correction of -3072 but it cannot because
          //  the input has 512 latency. Any wave1 playback material would be
          //  misaligned with the monitored input material.
          //

          // Examine any recording path, if desired.
          if(want_record_side && track->isLatencyInputTerminal())
          {
            const TrackLatencyInfo& li = track->getDominanceInfo(true);
            if(!li._canDominateOutputLatency)
              track->setCorrectionLatencyInfo(true, song_worst_latency);
          }
          
          // Examine any playback path.
          if(track->isLatencyOutputTerminal())
          {
            const TrackLatencyInfo& li = track->getDominanceInfo(false);
            if(!li._canDominateOutputLatency)
              track->setCorrectionLatencyInfo(false, song_worst_latency);
          }
        }      
        
  #if 1
        // This includes synthesizers.
        for(ciMidiDevice imd = mdl.cbegin(); imd != mdl.cend(); ++imd) 
        {
          MidiDevice* md = *imd;
          // Device not in use?
          if(md->midiPort() < 0 || md->midiPort() >= MusECore::MIDI_PORTS)
            continue;

          if(md->isLatencyOutputTerminalMidi(true /*capture*/))
          {
            // Grab the capture device's branch's dominance latency info.
            // This should already be cached from the dominance pass.
            const TrackLatencyInfo& li = md->getDominanceInfoMidi(true /*capture*/, false);
            if(!li._canDominateOutputLatency)
              md->setCorrectionLatencyInfoMidi(true /*capture*/, false, song_worst_latency);
          }

          if(md->isLatencyOutputTerminalMidi(false /*playback*/))
          {
            // Grab the playback device's branch's dominance latency info.
            // This should already be cached from the dominance pass.
            const TrackLatencyInfo& li = md->getDominanceInfoMidi(false /*playback*/, false);
            if(!li._canDominateOutputLatency)
              md->setCorrectionLatencyInfoMidi(false /*playback*/, false, song_worst_latency);
          }
        }
  #endif      

        //if(MusEGlobal::song->click())
        if(metro_settings->audioClickFlag)
        {
          if(metronome->isLatencyOutputTerminal())
          {
            // Grab the branch's dominance latency info.
            // This should already be cached from the dominance pass.
            const TrackLatencyInfo& li = metronome->getDominanceInfo(false);
            if(!li._canDominateOutputLatency)
              metronome->setCorrectionLatencyInfo(false, song_worst_latency);
          }
        }
        if(metro_settings->midiClickFlag)
        {
          if(metronome->isLatencyOutputTerminalMidi(false /*playback*/))
          {
            // Grab the branch's dominance latency info.
            // This should already be cached from the dominance pass.
            const TrackLatencyInfo& li = metronome->getDominanceInfoMidi(false /*playback*/, false);
            if(!li._canDominateOutputLatency)
              metronome->setCorrectionLatencyInfoMidi(false /*playback*/, false, song_worst_latency);
          }
        }


        //---------------------------------------------
        // PASS 3: Set initial output latencies:
        //---------------------------------------------

        for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it) 
        {
          track = *it;
          
          // If the track is for example a Wave Track, we must consider up to two contributing paths,
          //  the output (playback) side and the input (record) side which can pass through via monitoring.
          want_record_side = track->isMidiTrack() || track->type() == Track::WAVE;
          
          //---------------------------------------------
          // We are looking for the end points of branches.
          // This includes Audio Outputs, and open-ended branches
          //  that go nowhere or are effectively disconnected from
          //  their destination(s).
          // This caches its result in the track's _latencyInfo structure
          //  in the _isLatencyOutputTerminal member so it can be used
          //  faster in the correction pass or final pass.
          //---------------------------------------------
          
          // Examine any recording path, if desired.
          if(want_record_side && track->isLatencyInputTerminal())
          {
            // Gather the branch's dominance latency info.
  //           const TrackLatencyInfo& li = track->getDominanceLatencyInfo();
  //           const TrackLatencyInfo& li = track->getInputDominanceLatencyInfo();
            
            const TrackLatencyInfo& li = track->getDominanceLatencyInfo(true);
            
  //           // Gather the branch's dominance latency info.
  //           const TrackLatencyInfo& li = track->setCorrectionLatencyInfo(true, 0.0f);

  //           // If the branch can dominate, and this end-point allows it, and its latency value
  //           //  is greater than the current worst, overwrite the worst.
  //           if(track->canDominateEndPointLatency() &&
  //             li._canDominateOutputLatency &&
  //             li._outputLatency > song_worst_latency)
  //               song_worst_latency = li._outputLatency;
            // If the branch can dominate, and this end-point allows it, and its latency value
            //  is greater than the current worst, overwrite the worst.
            if(track->canDominateEndPointLatency() &&
              li._canDominateInputLatency &&
              li._inputLatency > song_worst_latency)
                song_worst_latency = li._inputLatency;
          }
          
          // Examine any playback path.
          if(track->isLatencyOutputTerminal())
          {
            // Gather the branch's dominance latency info.
  //           const TrackLatencyInfo& li = track->getDominanceLatencyInfo();
            const TrackLatencyInfo& li = track->getDominanceLatencyInfo(false);
  //           const TrackLatencyInfo& li = track->setCorrectionLatencyInfo(false, 0.0f);

            // If the branch can dominate, and this end-point allows it, and its latency value
            //  is greater than the current worst, overwrite the worst.
            if(track->canDominateEndPointLatency() &&
              li._canDominateOutputLatency &&
              li._outputLatency > song_worst_latency)
                song_worst_latency = li._outputLatency;
          }
          
  //         if(!track->isLatencyOutputTerminal())
  //           continue;
          
  //         // Gather the branch's dominance latency info.
  //         const TrackLatencyInfo& li = track->getDominanceLatencyInfo();
  //         // If the branch can dominate, and this end-point allows it, and its latency value
  //         //  is greater than the current worst, overwrite the worst.
  //         if(track->canDominateEndPointLatency() &&
  //            li._canDominateOutputLatency &&
  //            li._outputLatency > route_worst_latency)
  //           route_worst_latency = li._outputLatency;
        }      

  #if 1
        // This includes synthesizers.
        for(ciMidiDevice imd = mdl.cbegin(); imd != mdl.cend(); ++imd) 
        {
          MidiDevice* md = *imd;
          // Device not in use?
          if(md->midiPort() < 0 || md->midiPort() >= MusECore::MIDI_PORTS)
            continue;

          //---------------------------------------------
          // We are looking for the end points of branches.
          // This includes Audio Outputs, and open-ended branches
          //  that go nowhere or are effectively disconnected from
          //  their destination(s).
          // This caches its result in the track's _latencyInfo structure
          //  in the _isLatencyOutputTerminal member so it can be used
          //  faster in the correction pass or final pass.
          //---------------------------------------------
          
          // Examine any playback path on capture devices.
          if(md->isLatencyOutputTerminalMidi(true /*capture*/))
          {
            // Gather the branch's dominance latency info.
            const TrackLatencyInfo& li = md->getDominanceLatencyInfoMidi(true /*capture*/, false);
            // If the branch can dominate, and this end-point allows it, and its latency value
            //  is greater than the current worst, overwrite the worst.
            if(md->canDominateEndPointLatencyMidi(true /*capture*/) &&
              li._canDominateOutputLatency &&
              li._outputLatency > song_worst_latency)
                song_worst_latency = li._outputLatency;
          }

          // Examine any playback path on playback devices.
          if(md->isLatencyOutputTerminalMidi(false /*playback*/))
          {
            // Gather the branch's dominance latency info.
            const TrackLatencyInfo& li = md->getDominanceLatencyInfoMidi(false /*playback*/, false);
            // If the branch can dominate, and this end-point allows it, and its latency value
            //  is greater than the current worst, overwrite the worst.
            if(md->canDominateEndPointLatencyMidi(false /*playback*/) &&
              li._canDominateOutputLatency &&
              li._outputLatency > song_worst_latency)
                song_worst_latency = li._outputLatency;
          }
        }
  #endif      
        
        //if(MusEGlobal::song->click())
        if(metro_settings->audioClickFlag)
        {
  //         // Examine any recording path, if desired.
  //         if(metronome->isLatencyInputTerminal())
  //         {
  //           // Gather the branch's dominance latency info.
  //           const TrackLatencyInfo& li = metronome->getInputDominanceLatencyInfo();
  //           // If the branch can dominate, and this end-point allows it, and its latency value
  //           //  is greater than the current worst, overwrite the worst.
  //           if(metronome->canDominateEndPointLatency() &&
  //             li._canDominateOutputLatency &&
  //             li._outputLatency > song_worst_latency)
  //               song_worst_latency = li._outputLatency;
  //         }

          // Examine any playback path.
          if(metronome->isLatencyOutputTerminal())
          {
            // Gather the branch's dominance latency info.
  //           const TrackLatencyInfo& li = metronome->getDominanceLatencyInfo();
            const TrackLatencyInfo& li = metronome->getDominanceLatencyInfo(false);
  //           const TrackLatencyInfo& li = metronome->setCorrectionLatencyInfo(false, 0.0f);

            // If the branch can dominate, and this end-point allows it, and its latency value
            //  is greater than the current worst, overwrite the worst.
            if(metronome->canDominateEndPointLatency() &&
              li._canDominateOutputLatency &&
              li._outputLatency > song_worst_latency)
                song_worst_latency = li._outputLatency;
          }
        }
        if(metro_settings->midiClickFlag)
        {
          // Examine any playback path.
          if(metronome->isLatencyOutputTerminalMidi(false /*playback*/))
          {
            // Gather the branch's dominance latency info.
  //           const TrackLatencyInfo& li = metronome->getDominanceLatencyInfo();
            const TrackLatencyInfo& li = metronome->getDominanceLatencyInfoMidi(false /*playback*/, false);
  //           const TrackLatencyInfo& li = metronome->setCorrectionLatencyInfo(false, 0.0f);

            // If the branch can dominate, and this end-point allows it, and its latency value
            //  is greater than the current worst, overwrite the worst.
            if(metronome->canDominateEndPointLatencyMidi(false /*playback*/) &&
              li._canDominateOutputLatency &&
              li._outputLatency > song_worst_latency)
                song_worst_latency = li._outputLatency;
          }
        }
        
        
  //       //---------------------------------------------
  //       // PASS 2: Set correction values:
  //       //---------------------------------------------
  //
  //       // Now that we know the worst case latency,
  //       //  set all the branch correction values.
  //       for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it) 
  //       {
  // //         if((*it)->isMidiTrack())
  // //           continue;
  // //         atrack = static_cast<AudioTrack*>(*it);
  //         track = *it;
  //         
  //         // Grab the branch's dominance latency info.
  //         // This should already be cached from the dominance pass.
  // //         const TrackLatencyInfo& li = track->getDominanceLatencyInfo();
  //         const TrackLatencyInfo& li = track->getDominanceLatencyInfo(false);
  //         // We are looking for the end points of branches.
  //         if(!li._isLatencyOutputTerminal)
  //           continue;
  //         
  //         // Set branch correction values, for any tracks which support it.
  //         // If this branch can dominate latency, ie. is fed from an Audio Input Track,
  //         //  then we cannot apply correction to the branch.
  //         // For example:
  //         //
  //         //  Input(512) -> Wave1(monitor on) -> Output(3072)
  //         //                                  -> Wave2
  //         //
  //         // Wave1 wants to set a correction of -3072 but it cannot because
  //         //  the input has 512 latency. Any wave1 playback material would be
  //         //  misaligned with the monitored input material.
  //         //
  //         if(!li._canDominateOutputLatency)
  //             track->setCorrectionLatencyInfo(song_worst_latency);
  //       }      
  //       
  // #if 1
  //       // This includes synthesizers.
  //       for(ciMidiDevice imd = mdl.cbegin(); imd != mdl.cend(); ++imd) 
  //       {
  //         MidiDevice* md = *imd;
  //         // Device not in use?
  //         if(md->midiPort() < 0 || md->midiPort() >= MusECore::MIDI_PORTS)
  //           continue;
  // 
  //         // Grab the capture device's branch's dominance latency info.
  //         // This should already be cached from the dominance pass.
  //         const TrackLatencyInfo& cli = md->getDominanceLatencyInfoMidi(true /*capture*/);
  //         // We are looking for the end points of branches.
  //         if(!cli._isLatencyOutputTerminal)
  //           continue;
  //         
  //         // Set branch correction values, for any tracks which support it.
  //         // If this branch can dominate latency, ie. is fed from an Audio Input Track,
  //         //  then we cannot apply correction to the branch.
  //         // For example:
  //         //
  //         //  Input(512) -> Wave1(monitor on) -> Output(3072)
  //         //                                  -> Wave2
  //         //
  //         // Wave1 wants to set a correction of -3072 but it cannot because
  //         //  the input has 512 latency. Any wave1 playback material would be
  //         //  misaligned with the monitored input material.
  //         //
  //         if(!cli._canDominateOutputLatency)
  //             md->setCorrectionLatencyInfoMidi(true /*capture*/, song_worst_latency);
  // 
  //         // Grab the playback device's branch's dominance latency info.
  //         // This should already be cached from the dominance pass.
  //         const TrackLatencyInfo& pli = md->getDominanceLatencyInfoMidi(false /*playback*/);
  //         // We are looking for the end points of branches.
  //         if(!pli._isLatencyOutputTerminal)
  //           continue;
  //         
  //         if(!pli._canDominateOutputLatency)
  //             md->setCorrectionLatencyInfoMidi(false /*playback*/, song_worst_latency);
  //       }
  // #endif      
  // 
  //       //if(MusEGlobal::song->click())
  //       if(metro_settings->audioClickFlag)
  //       {
  //         // Grab the branch's dominance latency info.
  //         // This should already be cached from the dominance pass.
  // //         const TrackLatencyInfo& li = metronome->getDominanceLatencyInfo();
  //         const TrackLatencyInfo& li = metronome->getDominanceLatencyInfo(false);
  //         if(li._isLatencyOutputTerminal)
  //         {
  //           if(!li._canDominateOutputLatency)
  //             metronome->setCorrectionLatencyInfo(song_worst_latency);
  //         }
  //       }


        //----------------------------------------------------------
        // PASS 4: Gather final latency values and set compensators:
        //----------------------------------------------------------

        // Now that all branch correction values have been set,
        //  gather all final latency info.
        for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it) 
        {
  //         if((*it)->isMidiTrack())
  //           continue;
  //         atrack = static_cast<AudioTrack*>(*it);
          track = *it;
          
          // If the track is for example a Wave Track, we must consider up to two contributing paths,
          //  the output (playback) side and the input (record) side which can pass through via monitoring.
          want_record_side = track->isMidiTrack() || track->type() == Track::WAVE;

          // Examine any recording path, if desired.
          // We are looking for the end points of branches.
          if(want_record_side && track->isLatencyInputTerminal())
          {
  //           const TrackLatencyInfo& dli = track->getInputDominanceLatencyInfo();
  //           const TrackLatencyInfo& dli = track->getDominanceLatencyInfo(true);
  //           const TrackLatencyInfo& dli = track->setCorrectionLatencyInfo(true, 0.0f);

            // Gather the branch's final latency info, which also sets the
            //  latency compensators.
            //const TrackLatencyInfo& li = track->getLatencyInfo();
  //                 track->getInputLatencyInfo();
            track->getLatencyInfo(true);

            // Set this end point's latency compensator write offset.
            track->setLatencyCompWriteOffset(song_worst_latency);
          }

          // Examine any playback path.
          // We are looking for the end points of branches.
          if(track->isLatencyOutputTerminal())
          {
            // Grab the branch's dominance latency info.
            // This should already be cached from the dominance pass.
    //         const TrackLatencyInfo& dlo = track->getDominanceLatencyInfo();
    //         const TrackLatencyInfo& dlo = track->getDominanceLatencyInfo(false);
  //           const TrackLatencyInfo& dlo = track->setCorrectionLatencyInfo(false, 0.0f);

    //         // We are looking for the end points of branches.
    //         if(!dlo._isLatencyOutputTerminal)
    //           continue;
            
            // Gather the branch's final latency info, which also sets the
            //  latency compensators.
            //const TrackLatencyInfo& li = track->getLatencyInfo();
  //             track->getLatencyInfo();
            track->getLatencyInfo(false);
            
            // Set this end point's latency compensator write offset.
            track->setLatencyCompWriteOffset(song_worst_latency);
          }
        }      
        
  #if 1
        // This includes synthesizers.
        for(ciMidiDevice imd = mdl.cbegin(); imd != mdl.cend(); ++imd) 
        {
          MidiDevice* md = *imd;
          // Device not in use?
          if(md->midiPort() < 0 || md->midiPort() >= MusECore::MIDI_PORTS)
            continue;
          
          // We are looking for the end points of branches.
          if(md->isLatencyOutputTerminalMidi(true /*capture*/))
          {
            // Examine any playback path of the capture device.
            // Gather the branch's final latency info, which also sets the
            //  latency compensators.
            md->getLatencyInfoMidi(true /*capture*/, false);
            // Set this end point's latency compensator write offset.
            md->setLatencyCompWriteOffsetMidi(song_worst_latency, true /*capture*/);
          }

          // We are looking for the end points of branches.
          if(md->isLatencyOutputTerminalMidi(false /*playback*/))
          {
            // Examine any playback path of the playback device.
            // Gather the branch's final latency info, which also sets the
            //  latency compensators.
            md->getLatencyInfoMidi(false /*playback*/, false);
            // Set this end point's latency compensator write offset.
            md->setLatencyCompWriteOffsetMidi(song_worst_latency, false /*playback*/);
          }
        }
  #endif      

        //if(MusEGlobal::song->click())
        if(metro_settings->audioClickFlag)
        {
          // We are looking for the end points of branches.
          if(metronome->isLatencyOutputTerminal())
          {
            // Gather the branch's final latency info, which also sets the
            //  latency compensators.
            metronome->getLatencyInfo(false);
            // Set this end point's latency compensator write offset.
            metronome->setLatencyCompWriteOffset(song_worst_latency);
          }
        }
        if(metro_settings->midiClickFlag)
        {
          // We are looking for the end points of branches.
          if(metronome->isLatencyOutputTerminalMidi(false /*playback*/))
          {
            // Gather the branch's final latency info, which also sets the
            //  latency compensators.
            metronome->getLatencyInfoMidi(false /*playback*/, false);
            // Set this end point's latency compensator write offset.
            metronome->setLatencyCompWriteOffsetMidi(song_worst_latency, false /*playback*/);
          }
        }
      }


//       OutputList* ol = MusEGlobal::song->outputs();
//       const int rl_sz = ol->size();
//       unsigned long latency_array[rl_sz];
//       int latency_array_cnt = 0;
//       //float route_worst_latency = 0.0f;
//       for (ciAudioOutput i = ol->begin(); i != ol->end(); ++i, ++latency_array_cnt)
//       {
//         track = static_cast<AudioTrack*>(*i);
//         const TrackLatencyInfo& li = track->getLatencyInfo();
//         latency_array[latency_array_cnt] = li._outputLatency;
//         if(li._outputLatency > route_worst_latency)
//           route_worst_latency = li._outputLatency;
//       }
//       // Were ANY tracks unprocessed as a result of processing all the AudioOutputs, above? 
//       for(ciTrack it = tl->begin(); it != tl->end(); ++it) 
//       {
//         if((*it)->isMidiTrack())
//           continue;
//         track = static_cast<AudioTrack*>(*it);
//         if(track->type() == Track::AUDIO_OUTPUT)
//           continue;
//         track->getLatencyInfo();
//       }      
//       // Now prepare the latency array to be passed to the compensator's writer,
//       //  by adjusting each route latency value. ie. the route with the worst-case
//       //  latency will get ZERO delay, while routes having smaller latency will get
//       //  MORE delay, to match all the signals' timings together.
//       for(int i = 0; i < rl_sz; ++i)
//         latency_array[i] = route_worst_latency - latency_array[i];
      
      //---------------------------------------------
      // Midi processing
      //---------------------------------------------
      
      processMidi(frames);
      
      //---------------------------------------------
      // Audio processing
      //---------------------------------------------
      
      // Process Aux tracks first.
      for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it)
      {
        if((*it)->isMidiTrack())
          continue;
        atrack = static_cast<AudioTrack*>(*it);
        if(!atrack->processed() && atrack->type() == Track::AUDIO_AUX)
        {
          //fprintf(stderr, "Audio::process1 Do aux: track:%s\n", track->name().toLatin1().constData());   DELETETHIS
          channels = atrack->channels();
          // Just a dummy buffer.
          float* buffer[channels];
          float data[frames * channels];
          for (int i = 0; i < channels; ++i)
                buffer[i] = data + i * frames;
          //fprintf(stderr, "Audio::process1 calling track->copyData for track:%s\n", track->name().toLatin1()); DELETETHIS
            atrack->copyData(samplePos, -1, channels, channels, -1, -1, frames, buffer);
        }
      }
      
      // REMOVE Tim. latency. Changed.
      OutputList* ol = MusEGlobal::song->outputs();
      for (ciAudioOutput i = ol->cbegin(); i != ol->cend(); ++i)
        (*i)->process(samplePos, offset, frames);
//       // Reset.
//       latency_array_cnt = 0;
//       for (ciAudioOutput i = ol->begin(); i != ol->end(); ++i, ++latency_array_cnt)
//       {
//         AudioOutput* otrack = static_cast<AudioOutput*>(*i);
//         otrack->setLatencyCompWriteOffset(latency_array[latency_array_cnt]);
//         otrack->process(samplePos, offset, frames);
//       }
            
      // Were ANY tracks unprocessed as a result of processing all the AudioOutputs, above? 
      // Not just unconnected ones, as previously done, but ones whose output path ultimately leads nowhere.
      // Those tracks were missed, until this fix.
      // Do them now. This will animate meters, and 'quietly' process some audio which needs to be done -
      //  for example synths really need to be processed, 'quietly' or not, otherwise the next time processing 
      //  is 'turned on', if there was a backlog of events while it was off, then they all happen at once.  Tim.
      for(ciTrack it = tl->cbegin(); it != tl->cend(); ++it) 
      {
        if((*it)->isMidiTrack())
          continue;
        atrack = static_cast<AudioTrack*>(*it);
        if(!atrack->processed() && (atrack->type() != Track::AUDIO_OUTPUT))
        {
          //fprintf(stderr, "Audio::process1 track:%s\n", track->name().toLatin1().constData());  DELETETHIS
          channels = atrack->channels();
          // Just a dummy buffer.
          float* buffer[channels];
          float data[frames * channels];
          for (int i = 0; i < channels; ++i)
                buffer[i] = data + i * frames;
          // REMOVE Tim. latency. Added.
          
          //fprintf(stderr, "Audio::process1 calling track->copyData for track:%s\n", track->name().toLatin1()); DELETETHIS
            atrack->copyData(samplePos, -1, channels, channels, -1, -1, frames, buffer);
        }
      }      
    }

//---------------------------------------------------------
//   processMsg
//---------------------------------------------------------

void Audio::processMsg(AudioMsg* msg)
      {
      switch(msg->id) {
            case AUDIO_ROUTEADD:
                  addRoute(msg->sroute, msg->droute);
                  break;
            case AUDIO_ROUTEREMOVE:
                  removeRoute(msg->sroute, msg->droute);
                  break;
            case AUDIO_REMOVEROUTES:      
                  removeAllRoutes(msg->sroute, msg->droute);
                  break;
            case SEQM_SET_AUX:
                  msg->snode->setAuxSend(msg->ival, msg->dval);
                  break;
            case AUDIO_SET_PREFADER:
                  msg->snode->setPrefader(msg->ival);
                  break;
            case AUDIO_SET_CHANNELS:
                  msg->snode->setChannels(msg->ival);
                  break;
            case AUDIO_ADDPLUGIN:
                  msg->snode->addPlugin(msg->plugin, msg->ival);
                  break;
            case AUDIO_SWAP_CONTROLLER_IDX:
                  msg->snode->swapControllerIDX(msg->a, msg->b);
                  break;
            case AUDIO_CLEAR_CONTROLLER_EVENTS:
                  msg->snode->clearControllerEvents(msg->ival);
                  break;
            case AUDIO_SEEK_PREV_AC_EVENT:
                  msg->snode->seekPrevACEvent(msg->ival);
                  break;
            case AUDIO_SEEK_NEXT_AC_EVENT:
                  msg->snode->seekNextACEvent(msg->ival);
                  break;
            case AUDIO_ERASE_AC_EVENT:
                  msg->snode->eraseACEvent(msg->ival, msg->a);
                  break;
            case AUDIO_ERASE_RANGE_AC_EVENTS:
                  msg->snode->eraseRangeACEvents(msg->ival, msg->a, msg->b);
                  break;
            case AUDIO_ADD_AC_EVENT:
                  msg->snode->addACEvent(msg->ival, msg->a, msg->dval);
                  break;
            case AUDIO_CHANGE_AC_EVENT:
                  msg->snode->changeACEvent(msg->ival, msg->a, msg->b, msg->dval);
                  break;

            case AUDIO_SET_SEND_METRONOME:
                  msg->snode->setSendMetronome((bool)msg->ival);
                  break;
            
            case AUDIO_START_MIDI_LEARN:
                  // Reset the values. The engine will fill these from driver events.
                  MusEGlobal::midiLearnPort = -1;
                  MusEGlobal::midiLearnChan = -1;
                  MusEGlobal::midiLearnCtrl = -1;
                  break;

            case SEQM_RESET_DEVICES:
                  for (int i = 0; i < MusECore::MIDI_PORTS; ++i)                         
                  {      
                    if(MusEGlobal::midiPorts[i].device())                       
                      MusEGlobal::midiPorts[i].instrument()->reset(i);
                  }      
                  break;
            case SEQM_INIT_DEVICES:
                  initDevices(msg->a);
                  break;
            case SEQM_MIDI_LOCAL_OFF:
                  sendLocalOff();
                  break;
            case SEQM_PANIC:
                  panic();
                  break;
            case SEQM_PLAY_MIDI_EVENT:
                  {
                  const MidiPlayEvent ev = *((MidiPlayEvent*)(msg->p1));
                  const int port = ev.port();
                  if(port < 0 || port >= MusECore::MIDI_PORTS)
                    break;
                  
                  // This is the audio thread. Just set directly.
                  MusEGlobal::midiPorts[port].setHwCtrlState(ev);
                  // Send to the device.
                  if(MidiDevice* md = MusEGlobal::midiPorts[port].device())
                    md->putEvent(ev, MidiDevice::NotLate);
                  // Record??
                  }
                  break;
            case SEQM_SET_HW_CTRL_STATE:
                  {
                  MidiPort* port = (MidiPort*)(msg->p1);
                  port->setHwCtrlState(msg->a, msg->b, msg->c);
                  }
                  break;
            case SEQM_SET_HW_CTRL_STATES:
                  {
                  MidiPort* port = (MidiPort*)(msg->p1);
                  port->setHwCtrlStates(msg->a, msg->b, msg->c, msg->ival);
                  }
                  break;

            case SEQM_SET_TRACK_AUTO_TYPE:
                  msg->track->setAutomationType(AutomationType(msg->ival));
                  break;
                  
            case SEQM_IDLE:
                  idle = msg->a;
                  if(MusEGlobal::midiSeq)
                    MusEGlobal::midiSeq->sendMsg(msg);
                  break;

            case AUDIO_WAIT:
                  // Do nothing.
                  break;

            default:
                  MusEGlobal::song->processMsg(msg);
                  break;
            }
      }

//---------------------------------------------------------
//   seek
//    - called before start play
//    - initiated from gui
//    To be called from audio thread only.
//---------------------------------------------------------

void Audio::seek(const Pos& p)
      {
      if (_pos == p) {
            if(MusEGlobal::debugMsg)
              fprintf(stderr, "Audio::seek already at frame:%u\n", p.frame());
            return;        
            }
      if (MusEGlobal::heavyDebugMsg)
        fprintf(stderr, "Audio::seek frame:%d\n", p.frame());
        
#ifdef _AUDIO_USE_TRUE_FRAME_
      _previousPos = _pos;
#endif
      _pos        = p;
      if (!MusEGlobal::checkAudioDevice()) return;
      syncFrame   = MusEGlobal::audioDevice->framesAtCycleStart();
      
#ifdef _JACK_TIMEBASE_DRIVES_MIDI_
      unsigned curr_jt_tick;
      if(MusEGlobal::audioDevice->deviceType() == AudioDevice::JACK_AUDIO && 
         !MusEGlobal::jackTransportMaster && 
         !MusEGlobal::song->masterFlag() &&
         !MusEGlobal::extSyncFlag.value() &&
         static_cast<MusECore::JackAudioDevice*>(MusEGlobal::audioDevice)->timebaseQuery(
             MusEGlobal::segmentSize, NULL, NULL, NULL, &curr_jt_tick, NULL))
        curTickPos = curr_jt_tick;
      else
#endif
      curTickPos  = _pos.tick();

      // Now that curTickPos is set, set the correct initial metronome midiClick.
      updateMidiClick();
      
      //
      // Handle stuck notes and set controllers for new position:
      //

      seekMidi();
      
      if (state != LOOP2 && !freewheel())
      {
            // We need to force prefetch to update, to ensure the most recent data. 
            // Things can happen to a part before play is pressed - such as part muting, 
            //  part moving etc. Without a force, the wrong data was being played.  Tim 08/17/08
            // This does not wait.
            MusEGlobal::audioPrefetch->msgSeek(_pos.frame(), true);
      }
            
      write(sigFd, "G", 1);   // signal seek to gui
      }

//---------------------------------------------------------
//   writeTick
//    called from audio prefetch thread context
//    write another buffer to soundfile
//---------------------------------------------------------

void Audio::writeTick()
      {
      AudioOutput* ao = MusEGlobal::song->bounceOutput;
      if(ao && MusEGlobal::song->outputs()->find(ao) != MusEGlobal::song->outputs()->end())
      {
        if(ao->recordFlag())
          ao->record();
      }
      WaveTrackList* tl = MusEGlobal::song->waves();
      for (iWaveTrack t = tl->begin(); t != tl->end(); ++t) {
            WaveTrack* track = *t;
            if (track->recordFlag())
                  track->record();
            }
      }

//---------------------------------------------------------
//   startRolling
//---------------------------------------------------------

void Audio::startRolling()
      {
      if (MusEGlobal::debugMsg)
        fprintf(stderr, "startRolling - loopCount=%d, _pos=%d\n", _loopCount, _pos.tick());

      if(_loopCount == 0) {
        startRecordPos = _pos;
        startExternalRecTick = curTickPos;
      }
      if (MusEGlobal::song->record()) {
            recording      = true;
            WaveTrackList* tracks = MusEGlobal::song->waves();
            for (iWaveTrack i = tracks->begin(); i != tracks->end(); ++i) {
                        (*i)->resetMeter();
                  }
            }
      state = PLAY;
      write(sigFd, "1", 1);   // Play

      // Don't send if external sync is on. The master, and our sync routing system will take care of that.
      if(!MusEGlobal::extSyncFlag.value())
      {
        for(int port = 0; port < MusECore::MIDI_PORTS; ++port) 
        {
          MidiPort* mp = &MusEGlobal::midiPorts[port];
          MidiDevice* dev = mp->device();
          if(!dev)
            continue;
              
          MidiSyncInfo& si = mp->syncInfo();
            
          if(si.MMCOut())
            mp->sendMMCDeferredPlay();
          
          if(si.MRTOut())
          {
            if(curTickPos)
              mp->sendContinue();
            else
              mp->sendStart();
          }  
        }
      }  

      // Set the correct initial metronome midiClick.
      // Theoretically shouldn't have to do this here, in seek() should be enough.
      // But just in case, no harm.
      updateMidiClick();
      
      // re-enable sustain 
      for (int i = 0; i < MusECore::MIDI_PORTS; ++i) {
          MidiPort* mp = &MusEGlobal::midiPorts[i];
          if(!mp->device())
            continue;
          for (int ch = 0; ch < MusECore::MUSE_MIDI_CHANNELS; ++ch) {
              if (mp->hwCtrlState(ch, CTRL_SUSTAIN) == 127) {
                        const MidiPlayEvent ev(0, i, ch, ME_CONTROLLER, CTRL_SUSTAIN, 127);
                        mp->device()->putEvent(ev, MidiDevice::NotLate);
                  }
              }
          }
     }

//---------------------------------------------------------
//   stopRolling
//---------------------------------------------------------

void Audio::stopRolling()
{
      if (MusEGlobal::debugMsg)
        fprintf(stderr, "Audio::stopRolling state %s\n", audioStates[state]);
      
      state = STOP;

      //
      // Clear midi device notes and stop stuck notes:
      //

      // Clear the special sync play state (separate from audio play state).
      MusEGlobal::midiSyncContainer.setExternalPlayState(ExtMidiClock::ExternStopped); // Not playing.   Moved here from MidiSeq::processStop()

      // Stop the ALSA devices...
      if(MusEGlobal::midiSeq)
        MusEGlobal::midiSeq->msgStop();  // FIXME: This waits!

      // Stop any non-ALSA devices...
      for(iMidiDevice id = MusEGlobal::midiDevices.begin(); id != MusEGlobal::midiDevices.end(); ++id)
      {
        MidiDevice* md = *id;
        const MidiDevice::MidiDeviceType type = md->deviceType();
        // Only for non-ALSA devices.
        switch(type)
        {
          case MidiDevice::ALSA_MIDI:
          break;

          case MidiDevice::JACK_MIDI:
          case MidiDevice::SYNTH_MIDI:
            md->handleStop();
          break;
        }
      }

      // There may be disk read/write fifo buffers waiting to be emptied. Send one last tick to the disk thread.
      if(!freewheel())
        MusEGlobal::audioPrefetch->msgTick(recording, false); // This does not wait.
      
      WaveTrackList* tracks = MusEGlobal::song->waves();
      for (iWaveTrack i = tracks->begin(); i != tracks->end(); ++i) {
            (*i)->resetMeter();
            }
      recording    = false;
      endRecordPos = _pos;
      endExternalRecTick = curTickPos;
      write(sigFd, "0", 1);   // STOP
      }

//---------------------------------------------------------
//   recordStop
//    execution environment: gui thread
//---------------------------------------------------------

void Audio::recordStop(bool restart, Undo* ops)
      {
      MusEGlobal::song->processMasterRec();   
        
      if (MusEGlobal::debugMsg)
        fprintf(stderr, "recordStop - startRecordPos=%d\n", MusEGlobal::extSyncFlag.value() ? startExternalRecTick : startRecordPos.tick());

      Undo loc_ops;
      Undo& operations = ops ? (*ops) : loc_ops;
      
      WaveTrackList* wl = MusEGlobal::song->waves();

      for (iWaveTrack it = wl->begin(); it != wl->end(); ++it) {
            WaveTrack* track = *it;
            if (track->recordFlag() || MusEGlobal::song->bounceTrack == track) {
                  MusEGlobal::song->cmdAddRecordedWave(track, startRecordPos, restart ? _pos : endRecordPos, operations);
                  if(!restart)
                    operations.push_back(UndoOp(UndoOp::SetTrackRecord, track, false, true)); // True = non-undoable.
                  }
            }
      MidiTrackList* ml = MusEGlobal::song->midis();
      for (iMidiTrack it = ml->begin(); it != ml->end(); ++it) {
            MidiTrack* mt     = *it;

            //---------------------------------------------------
            //    resolve NoteOff events, Controller etc.
            //---------------------------------------------------

            // Do SysexMeta. Do loops.
            buildMidiEventList(&mt->events, mt->mpevents, mt, MusEGlobal::config.division, true, true);
            MusEGlobal::song->cmdAddRecordedEvents(mt, mt->events, 
                 MusEGlobal::extSyncFlag.value() ? startExternalRecTick : startRecordPos.tick(),
                 operations);
            mt->events.clear();    // ** Driver should not be touching this right now.
            mt->mpevents.clear();  // ** Driver should not be touching this right now.
            }
      
      //
      // bounce to file operates on the only
      // selected output port
      //
      
      AudioOutput* ao = MusEGlobal::song->bounceOutput;
      if(ao && MusEGlobal::song->outputs()->find(ao) != MusEGlobal::song->outputs()->end())
      {
        if(ao->recordFlag())
        {            
          MusEGlobal::song->bounceOutput = 0;
          ao->setRecFile(NULL); // if necessary, this automatically deletes _recFile
          operations.push_back(UndoOp(UndoOp::SetTrackRecord, ao, false, true));  // True = non-undoable.
        }
      }  

      // Operate on a local list if none was given.
      if(!ops)
        MusEGlobal::song->applyOperationGroup(loc_ops);
      
      if(!restart)
         MusEGlobal::song->setRecord(false);
      }

//---------------------------------------------------------
//   framesAtCycleStart  
//    Frame count at the start of current cycle. 
//    This is meant to be called from inside process thread only.      
//---------------------------------------------------------

unsigned Audio::framesAtCycleStart() const
{
      return MusEGlobal::audioDevice->framesAtCycleStart();  
}

//---------------------------------------------------------
//   framesSinceCycleStart
//    Estimated frames since the last process cycle began
//    This can be called from outside process thread.
//---------------------------------------------------------

unsigned Audio::framesSinceCycleStart() const
{
  // Do not round up here since time resolution is higher than (audio) frame resolution.
  unsigned f = muse_multiply_64_div_64_to_64(curTimeUS() - syncTimeUS, MusEGlobal::sampleRate, 1000000UL);
  
  // Safety due to inaccuracies. It cannot be after the segment, right?
  if(f >= MusEGlobal::segmentSize)
    f = MusEGlobal::segmentSize - 1;
  return f;
  
  // REMOVE Tim. Or keep? (During midi_engine_fixes.) 
  // Can't use this since for the Jack driver, jack_frames_since_cycle_start is designed to be called ONLY from inside process.
  // return MusEGlobal::audioDevice->framesSinceCycleStart();   
}

//---------------------------------------------------------
//   curFramePos()
//    Current play position frame. Estimated to single-frame resolution while in play mode.
//    This can be called from outside process thread.
//---------------------------------------------------------

unsigned Audio::curFramePos() const
{
  return _pos.frame() + (isPlaying() ? framesSinceCycleStart() : 0);
}

//---------------------------------------------------------
//   curFrame
//    Extrapolates current play frame on syncTime/syncFrame
//    Estimated to single-frame resolution.
//    This is an always-increasing number. Good for timestamps, and 
//     handling them during process when referenced to syncFrame.
//    This is meant to be called from threads other than the process thread.
//---------------------------------------------------------

unsigned int Audio::curFrame() const
      {
      return MusEGlobal::audioDevice->framePos();  
      
      // REMOVE Tim. Or keep? (During midi_engine_fixes.) 
      // Can't use this since for the Jack driver, jack_frames_since_cycle_start is designed to be called ONLY from inside process.
      //return framesAtCycleStart() + framesSinceCycleStart(); 
      }

//---------------------------------------------------------
//   updateMidiClick
//   Updates the metronome tick.
//   Useful for after seek(), even startRolling() etc.
//   To be called from audio thread only.
//---------------------------------------------------------

void Audio::updateMidiClick()
{
  // Set the correct initial metronome midiClick.
  int bar, beat;
  unsigned tick;
  MusEGlobal::sigmap.tickValues(curTickPos, &bar, &beat, &tick);
  if(tick)
    beat += 1;
  midiClick = MusEGlobal::sigmap.bar2tick(bar, beat, 0);
  // REMOVE Tim. latency. Added. TODO: Account for latency, in both the midiClick above and audioClick below !
  audioClick = midiClick;
}

//---------------------------------------------------------
//   sendMsgToGui
//---------------------------------------------------------

void Audio::sendMsgToGui(char c)
      {
      write(sigFd, &c, 1);
      }

} // namespace MusECore
