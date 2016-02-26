/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 2.1 of the License, or (at your

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2015, Live Networks, Inc.  All rights reserved
// A demo application, showing how to create and run a RTSP client (that can potentially receive multiple streams concurrently).
//
// NOTE: This code - although it builds a running application - is intended only to illustrate how to develop your own RTSP
// client application.  For a full-featured RTSP client application - with much more functionality, and many options - see
// "openRTSP": http://www.live555.com/openRTSP/

#include <jni.h>
#include <android/log.h>
#include <pthread.h>

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"

extern "C" 
{
    #define INBUF_CNT 32 //max frame list count

    #define AAC 1
    #define PCMU 2
    #define PCMA 3
    #define AUDIO_UNSUPPORT 0

    #define ANDROID_LIBBASE_FNAME(f) Java_com_via_rtsp_RTSPCodec_##f
    #define LIBBASE_API_in(fname, arg...) ANDROID_LIBBASE_FNAME(fname) (JNIEnv*  env, jobject obj, arg)
    #define LIBBASE_API(fname, arg...) LIBBASE_API_in(fname, arg)
    #define debug_printf(...) __android_log_print(ANDROID_LOG_DEBUG,"via_rtsp_jni" ,__VA_ARGS__)

    #define RECREATE_SPSPPS_BUFFERS true
    #define KILL_VUI true
    JavaVM * gs_jvm;
}

#define ADD_QUEUE_VERSION 1

// Forward function definitions:

// RTSP 'response handlers':
void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString);
void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString);

// Other event handler functions:
void subsessionAfterPlaying(void* clientData); // called when a stream's subsession (e.g., audio or video substream) ends
void subsessionByeHandler(void* clientData); // called when a RTCP "BYE" is received for a subsession
void streamTimerHandler(void* clientData);
  // called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

// The main streaming routine (for each "rtsp://" URL):
void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL);

// Used to iterate through each stream's 'subsessions', setting up each one:
void setupNextSubsession(RTSPClient* rtspClient);

// Used to shut down and close a stream (including its "RTSPClient" object):
void shutdownStream(RTSPClient* rtspClient, int exitCode = 1);

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
  return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
  return env << subsession.mediumName() << "/" << subsession.codecName();
}

void usage(UsageEnvironment& env, char const* progName) {
  env << "Usage: " << progName << " <rtsp-url-1> ... <rtsp-url-N>\n";
  env << "\t(where each <rtsp-url-i> is a \"rtsp://\" URL)\n";
}



// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState {
public:
  StreamClientState();
  virtual ~StreamClientState();

public:
  MediaSubsessionIterator* iter;
  MediaSession* session;
  MediaSubsession* subsession;
  TaskToken streamTimerTask;
  double duration;
};

// If you're streaming just a single stream (i.e., just from a single URL, once), then you can define and use just a single
// "StreamClientState" structure, as a global variable in your application.  However, because - in this demo application - we're
// showing how to play multiple streams, concurrently, we can't do that.  Instead, we have to have a separate "StreamClientState"
// structure for each "RTSPClient".  To do this, we subclass "RTSPClient", and add a "StreamClientState" field to the subclass:

class ourRTSPClient: public RTSPClient {
public:
  static ourRTSPClient* createNew(UsageEnvironment& env, char const* rtspURL,
                  int verbosityLevel = 0,
                  char const* applicationName = NULL,
                  portNumBits tunnelOverHTTPPortNum = 0);

protected:
  ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
        int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum);
    // called only by createNew();
  virtual ~ourRTSPClient();

public:
  StreamClientState scs;



public:
  int check_structure_code;
  char eventLoopWatchVariable;
  JNIEnv* jenv;
  jobject jobject_rtspcodec;
};


typedef struct bit_stream_dec
{
    // CABAC Decoding
    int           read_len;           //!< actual position in the codebuffer, CABAC only
    int           code_len;           //!< overall codebuffer length, CABAC only
    // CAVLC Decoding
    int           frame_bitoffset;    //!< actual position in the codebuffer, bit-oriented, CAVLC only
    int           bitstream_length;   //!< over codebuffer lnegth, byte oriented, CAVLC only
    // ErrorConcealment
    unsigned char *streamBuffer;      //!< actual codebuffer for read bytes
    int           ei_flag;            //!< error indication, 0: no error, else unspecified error

}Bitstream;

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').
// In practice, this might be a class (or a chain of classes) that decodes and then renders the incoming audio or video.
// Or it might be a "FileSink", for outputting the received data into a file (as is done by the "openRTSP" application).
// In this example code, however, we define a simple 'dummy' sink that receives incoming data, but does nothing with it.

class DummySink: public MediaSink {
public:
  static DummySink* createNew(UsageEnvironment& env,
                  MediaSubsession& subsession, // identifies the kind of data that's being received
                  char const* streamId = NULL); // identifies the stream itself (optional)

private:
  DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId);
    // called only by "createNew()"
  virtual ~DummySink();

  static void afterGettingFrame(void* clientData, unsigned frameSize,
                                unsigned numTruncatedBytes,
                struct timeval presentationTime,
                                unsigned durationInMicroseconds);
  void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
             struct timeval presentationTime, unsigned durationInMicroseconds);

  int ProcessH264SPS(unsigned char* buffer, int length);
  int read_u_v (int LenInBits, const char* tracestring, Bitstream *bitstream, int *used_bits);
  int read_ue_v (const char *tracestring, Bitstream *bitstream, int *used_bits);
  int read_se_v (const char *tracestring, Bitstream *bitstream, int *used_bits);
  void linfo_ue(int len, int info, int *value1, int *dummy);
  void linfo_se(int len, int info, int *value1, int *dummy);
  int GetBits (unsigned char buffer[], int totbitoffset, int *info, int bitcount, int numbits);
  int GetVLCSymbol (unsigned char buffer[],int totbitoffset,int *info, int bytecount);
  void Scaling_List(int sizeOfScalingList, Bitstream *s, int *UsedBits);


  void write_u_v (const char* tracestring, Bitstream *bitstream, int *used_bits, int setLenInBit, int setNum);

private:
  // redefined virtual functions:
  virtual Boolean continuePlaying();

private:
  u_int8_t* fReceiveBuffer;
  MediaSubsession& fSubsession;
  char* fStreamId;
  u_int8_t fNeedToSetVideoMediaCodec;
  u_int8_t fReceivedIDR;

  u_int8_t fNeedToSetAudioMediaCodec;

  int fH264_level_idc;
  int fH264_width;
  int fH264_height;



public:
  void initJni(JNIEnv* jnienv, jobject obj);

private:
  bool bJniMethodInited;
  JNIEnv *jenv; // bypass from ourRTSPClient.
  jobject jobject_rtspcodec; // bypass from ourRTSPClient, do not delete reference here!

  jmethodID methodID_vdecode; //callback to update vdecode
  jmethodID methodID_SetVideoMediaCodec; //callback to methodID_SetVideoMediaCodec
  jmethodID methodID_InputVideoDirectBuffer; //callback to InputVideoDirectBufferCallback
  jmethodID methodID_AskDequeueVideoInputBufferIdx; //callback to AskDequeueVideoInputBufferIdxCallback
  jmethodID methodID_SPSDirectBuffer; //callback to SPSDirectBufferCallback
  jmethodID methodID_PPSDirectBuffer; //callback to PPSDirectBufferCallback

  jmethodID methodID_adecode; //callback to update adecode
  jmethodID methodID_InputAudioDirectBuffer; //callback to InputAudioDirectBufferCallback
  jmethodID methodID_AskDequeueAudioInputBufferIdx; //callback to AskDequeueAudioInputBufferIdxCallback
  jmethodID methodID_SetAudioMediaCodec; //callback to methodID_SetAudioMediaCodec
  jmethodID methodID_ESDSDirectBuffer; //callback to ESDSDirectBufferCallback

  jfieldID fieldID_spsBuffer;
  jfieldID fieldID_ppsBuffer;

  unsigned char * pvAudioDirectBufferFromJava[INBUF_CNT];
  unsigned char * pvVideoDirectBufferFromJava[INBUF_CNT];
  
  //////////////////////////////////////////////////////
  #ifdef ADD_QUEUE_VERSION
  jmethodID methodID_GetDirectInputVideoBuffer; //callback to GetDirectInputVideoBuffer
  jmethodID methodID_FillInputVideoBufferDone; //callback to FillInputBufferDoneCallBack
  #endif

  //////////////////////////////////////////////////////  
};

#define RTSP_CLIENT_VERBOSITY_LEVEL 1 // by default, print verbose output from each "RTSPClient"

static unsigned rtspClientCount = 0; // Counts how many streams (i.e., "RTSPClient"s) are currently in use.

void openURL(UsageEnvironment& env, char const* progName, char const* rtspURL) {
  // Begin by creating a "RTSPClient" object.  Note that there is a separate "RTSPClient" object for each stream that we wish
  // to receive (even if more than stream uses the same "rtsp://" URL).
  RTSPClient* rtspClient = ourRTSPClient::createNew(env, rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, progName);
  if (rtspClient == NULL) {
    env << "Failed to create a RTSP client for URL \"" << rtspURL << "\": " << env.getResultMsg() << "\n";
    //debug_printf("Failed to create a RTSP client for URL\n");
    return;
  }

  ++rtspClientCount;

  // Next, send a RTSP "DESCRIBE" command, to get a SDP description for the stream.
  // Note that this command - like all RTSP commands - is sent asynchronously; we do not block, waiting for a response.
  // Instead, the following function call returns immediately, and we handle the RTSP response later, from within the event loop:
  rtspClient->sendDescribeCommand(continueAfterDESCRIBE); 
  //debug_printf("openURL end\n");
}


// Implementation of the RTSP 'response handlers':

void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
      delete[] resultString;
      break;
    }

    char* const sdpDescription = resultString;
    env << *rtspClient << "Got a SDP description:\n" << sdpDescription << "\n";

    // Create a media session object from this SDP description:
    scs.session = MediaSession::createNew(env, sdpDescription);
    delete[] sdpDescription; // because we don't need it anymore
    if (scs.session == NULL) {
      env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
      break;
    } else if (!scs.session->hasSubsessions()) {
      env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
      break;
    }

    // Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
    // calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
    // (Each 'subsession' will have its own data source.)
    scs.iter = new MediaSubsessionIterator(*scs.session);
    setupNextSubsession(rtspClient);
    return;
  } while (0);

  // An unrecoverable error occurred with this stream.
  shutdownStream(rtspClient);
}

// By default, we request that the server stream its data using RTP/UDP.
// If, instead, you want to request that the server stream via RTP-over-TCP, change the following to True:
#define REQUEST_STREAMING_OVER_TCP False

void setupNextSubsession(RTSPClient* rtspClient) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  scs.subsession = scs.iter->next();
  if (scs.subsession != NULL) {
    if (!scs.subsession->initiate()) {
      env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
      setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
    } else {
      env << *rtspClient << "Initiated the \"" << *scs.subsession << "\" subsession (";
      if (scs.subsession->rtcpIsMuxed()) {
    env << "client port " << scs.subsession->clientPortNum();
      } else {
    env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
      }
      env << ")\n";

      // Continue setting up this subsession, by sending a RTSP "SETUP" command:
      rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
    }
    return;
  }

  // We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
  if (scs.session->absStartTime() != NULL) {
    // Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
  } else {
    scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
    rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
  }
}

void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
      break;
    }

    env << *rtspClient << "Set up the \"" << *scs.subsession << "\" subsession (";
    if (scs.subsession->rtcpIsMuxed()) {
      env << "client port " << scs.subsession->clientPortNum();
    } else {
      env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
    }
    env << ")\n";

    // Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
    // (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
    // after we've sent a RTSP "PLAY" command.)

    scs.subsession->sink = DummySink::createNew(env, *scs.subsession, rtspClient->url());
      // perhaps use your own custom "MediaSink" subclass instead
    if (scs.subsession->sink == NULL) {
      env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
      << "\" subsession: " << env.getResultMsg() << "\n";
      break;
    }

    env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
    scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession 
    scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
                       subsessionAfterPlaying, scs.subsession);
    // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
    if (scs.subsession->rtcpInstance() != NULL) {
      scs.subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, scs.subsession);
    }
  } while (0);
  delete[] resultString;

  // Set up the next subsession, if any:
  setupNextSubsession(rtspClient);
}

void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
  Boolean success = False;

  do {
    UsageEnvironment& env = rtspClient->envir(); // alias
    StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

    if (resultCode != 0) {
      env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
      break;
    }

    // Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
    // using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
    // 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
    // (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
    if (scs.duration > 0) {
      unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
      scs.duration += delaySlop;
      unsigned uSecsToDelay = (unsigned)(scs.duration*1000000);
      scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, rtspClient);
    }

    env << *rtspClient << "Started playing session";
    if (scs.duration > 0) {
      env << " (for up to " << scs.duration << " seconds)";
    }
    env << "...\n";

    success = True;
  } while (0);
  delete[] resultString;

  if (!success) {
    // An unrecoverable error occurred with this stream.
    shutdownStream(rtspClient);
  }
}


// Implementation of the other event handlers:

void subsessionAfterPlaying(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

  // Begin by closing this subsession's stream:
  Medium::close(subsession->sink);
  subsession->sink = NULL;

  // Next, check whether *all* subsessions' streams have now been closed:
  MediaSession& session = subsession->parentSession();
  MediaSubsessionIterator iter(session);
  while ((subsession = iter.next()) != NULL) {
    if (subsession->sink != NULL) return; // this subsession is still active
  }

  // All subsessions' streams have now been closed, so shutdown the client:
  shutdownStream(rtspClient);
}

void subsessionByeHandler(void* clientData) {
  MediaSubsession* subsession = (MediaSubsession*)clientData;
  RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
  UsageEnvironment& env = rtspClient->envir(); // alias

  env << *rtspClient << "Received RTCP \"BYE\" on \"" << *subsession << "\" subsession\n";

  // Now act as if the subsession had closed:
  subsessionAfterPlaying(subsession);
}

void streamTimerHandler(void* clientData) {
  ourRTSPClient* rtspClient = (ourRTSPClient*)clientData;
  StreamClientState& scs = rtspClient->scs; // alias

  scs.streamTimerTask = NULL;

  // Shut down the stream:
  shutdownStream(rtspClient);
}

void shutdownStream(RTSPClient* rtspClient, int exitCode) {
  UsageEnvironment& env = rtspClient->envir(); // alias
  StreamClientState& scs = ((ourRTSPClient*)rtspClient)->scs; // alias

  // First, check whether any subsessions have still to be closed:
  if (scs.session != NULL) { 
    Boolean someSubsessionsWereActive = False;
    MediaSubsessionIterator iter(*scs.session);
    MediaSubsession* subsession;

    while ((subsession = iter.next()) != NULL) {
      if (subsession->sink != NULL) {
    Medium::close(subsession->sink);
    subsession->sink = NULL;

    if (subsession->rtcpInstance() != NULL) {
      subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
    }

    someSubsessionsWereActive = True;
      }
    }

    if (someSubsessionsWereActive) {
      // Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
      // Don't bother handling the response to the "TEARDOWN".
      rtspClient->sendTeardownCommand(*scs.session, NULL);
    }
  }

  env << *rtspClient << "Closing the stream.\n";
  Medium::close(rtspClient);
    // Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.

  if (--rtspClientCount == 0) {
    // The final stream has ended, so exit the application now.
    // (Of course, if you're embedding this code into your own application, you might want to comment this out,
    // and replace it with "eventLoopWatchVariable = 1;", so that we leave the LIVE555 event loop, and continue running "main()".)
    exit(exitCode);
  }
}


// Implementation of "ourRTSPClient":

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL,
                    int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum) {
  return new ourRTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
                 int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env,rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum, -1) {
	check_structure_code = 0xC0DECD0E;
	eventLoopWatchVariable = 0;
	jenv = NULL;
	jobject_rtspcodec = NULL;
}

ourRTSPClient::~ourRTSPClient() {
	if (jobject_rtspcodec != NULL) {
		jenv->DeleteGlobalRef(jobject_rtspcodec);
	}
}

// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
  : iter(NULL), session(NULL), subsession(NULL), streamTimerTask(NULL), duration(0.0) {
}

StreamClientState::~StreamClientState() {
  delete iter;
  if (session != NULL) {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment& env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    Medium::close(session);
  }
}


// Implementation of "DummySink":

// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 900000

DummySink* DummySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId) {
  return new DummySink(env, subsession, streamId);
}

DummySink::DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId)
  : MediaSink(env),
    fSubsession(subsession) {
  fStreamId = strDup(streamId);
  fReceiveBuffer = new u_int8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE];
  fNeedToSetVideoMediaCodec = 1;
  fNeedToSetAudioMediaCodec = 1;
  fReceivedIDR = 0;

  // clean jni medthods.
  bJniMethodInited = false;
  jenv = NULL;
  jobject_rtspcodec = NULL;
  methodID_vdecode = NULL;
  methodID_SetVideoMediaCodec = NULL;
  methodID_InputVideoDirectBuffer = NULL;
  methodID_AskDequeueVideoInputBufferIdx = NULL;
  methodID_SPSDirectBuffer = NULL;
  methodID_PPSDirectBuffer = NULL;

  methodID_adecode = NULL;
  methodID_InputAudioDirectBuffer = NULL;
  methodID_AskDequeueAudioInputBufferIdx = NULL;
  methodID_SetAudioMediaCodec = NULL;
  methodID_ESDSDirectBuffer = NULL; //callback to ESDSDirectBufferCallback

  fieldID_spsBuffer = NULL;
  fieldID_ppsBuffer = NULL;
  for (int i = 0; i < INBUF_CNT; i++)
  {
    pvVideoDirectBufferFromJava[i] = NULL;
    pvAudioDirectBufferFromJava[i] = NULL;
  }
  
  
  #ifdef ADD_QUEUE_VERSION
  methodID_GetDirectInputVideoBuffer = NULL;
  methodID_FillInputVideoBufferDone = NULL;
  #endif  
  
}


DummySink::~DummySink() {
  delete[] fReceiveBuffer;
  delete[] fStreamId;
}

void DummySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
                  struct timeval presentationTime, unsigned durationInMicroseconds) {
  DummySink* sink = (DummySink*)clientData;
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}


//---------

// can only write 1 bit!!!
void DummySink::write_u_v (const char* tracestring, Bitstream *bitstream, int *used_bits, int setLenInBit, int setNum)
{
    int totalbitlength  = (bitstream->bitstream_length << 3) + 7;
    if ((bitstream->frame_bitoffset + 1 ) <= totalbitlength)
    {
        int bitoffset  = 7 - (bitstream->frame_bitoffset & 0x07); // bit from start of byte
        int byteoffset = (bitstream->frame_bitoffset >> 3); // byte from start of buffer
        unsigned char* curbyte  = &(bitstream->streamBuffer[byteoffset]);
        unsigned char assign;

        if (setLenInBit == 1)
        {
            assign = (*curbyte);
            assign = setNum ? assign | (1 << bitoffset) : assign & ~(1 << bitoffset);
            //debug_printf(" write_u_v : *curbyte = %d --> %d", *curbyte, assign);
            (*curbyte) = assign;
        }

        bitstream->frame_bitoffset += setLenInBit; // move bitstream pointer
		*used_bits += setLenInBit;
    }
}

int DummySink::GetBits (unsigned char buffer[], int totbitoffset, int *info, int bitcount, int numbits)
{
    if ((totbitoffset + numbits ) > bitcount) 
    {
        return -1;
    }
    else
    {
        int bitoffset  = 7 - (totbitoffset & 0x07); // bit from start of byte
        int byteoffset = (totbitoffset >> 3); // byte from start of buffer
        int bitcounter = numbits;
        unsigned char* curbyte  = &(buffer[byteoffset]);
        int inf = 0;

        while (numbits--)
        {
            inf <<=1;    
            inf |= ((*curbyte)>> (bitoffset--)) & 0x01;    
            if (bitoffset == -1 ) 
            { //Move onto next byte to get all of numbits
                curbyte++;
                bitoffset = 7;
            }
            // Above conditional could also be avoided using the following:
            // curbyte   -= (bitoffset >> 3);
            // bitoffset &= 0x07;
        }
        *info = inf;

        return bitcounter;           // return absolute offset in bit from start of frame
    }
}

int DummySink::GetVLCSymbol (unsigned char buffer[],int totbitoffset,int *info, int bytecount)
{
    long byteoffset = (totbitoffset >> 3);         // byte from start of buffer
    int  bitoffset  = (7 - (totbitoffset & 0x07)); // bit from start of byte
    int  bitcounter = 1;
    int  len        = 0;
    unsigned char *cur_byte  = &(buffer[byteoffset]);
    int  ctr_bit    = ((*cur_byte) >> (bitoffset)) & 0x01;  // control bit for current bit posision

    while (ctr_bit == 0)
    {                 // find leading 1 bit
        len++;
        bitcounter++;
        bitoffset--;
        bitoffset &= 0x07;
        cur_byte  += (bitoffset == 7);
        byteoffset+= (bitoffset == 7);      
        ctr_bit    = ((*cur_byte) >> (bitoffset)) & 0x01;
    }

    if (byteoffset + ((len + 7) >> 3) > bytecount)
        return -1;
    else
    {
        // make infoword
        int inf = 0;                          // shortest possible code is 1, then info is always 0    

        while (len--)
        {
            bitoffset --;    
            bitoffset &= 0x07;
            cur_byte  += (bitoffset == 7);
            bitcounter++;
            inf <<= 1;    
            inf |= ((*cur_byte) >> (bitoffset)) & 0x01;
        }
        *info = inf;
        return bitcounter;           // return absolute offset in bit from start of frame
    }
}

typedef struct syntaxelement_dec
{
    int           type;                  //!< type of syntax element for data part.
    int           value1;                //!< numerical value of syntax element
    int           value2;                //!< for blocked symbols, e.g. run/level
    int           len;                   //!< length of code
    int           inf;                   //!< info part of CAVLC code
    unsigned int  bitpattern;            //!< CAVLC bitpattern
    int           context;               //!< CABAC context
    int           k;                     //!< CABAC context for coeff_count,uv

    //! for mapping of CAVLC to syntaxElement
    void  (DummySink::*mapping)(int len, int info, int *value1, int *value2);
    //! used for CABAC: refers to actual coding method of each individual syntax element type
    ///	void  (*reading)(struct macroblock_dec *currMB, struct syntaxelement_dec *, DecodingEnvironmentPtr);
} SyntaxElement;

void DummySink::linfo_ue(int len, int info, int *value1, int *dummy)
{
    //assert ((len >> 1) < 32);
    *value1 = (int) (((unsigned int) 1 << (len >> 1)) + (unsigned int) (info) - 1);
}

void DummySink::linfo_se(int len,  int info, int *value1, int *dummy)
{
    //assert ((len >> 1) < 32);
    unsigned int n = ((unsigned int) 1 << (len >> 1)) + (unsigned int) info - 1;
    *value1 = (n + 1) >> 1;
    if((n & 0x01) == 0)                           // lsb is signed bit
        *value1 = -*value1;
}

int DummySink::read_u_v (int LenInBits, const char* tracestring, Bitstream *bitstream, int *used_bits)
{
    SyntaxElement symbol;
    symbol.inf = 0;

    symbol.type = 0;//SE_HEADER;
    symbol.mapping = &DummySink::linfo_ue;   // Mapping rule
    symbol.len = LenInBits;

    int BitstreamLengthInBits  = (bitstream->bitstream_length << 3) + 7;

    if ((GetBits(bitstream->streamBuffer, bitstream->frame_bitoffset, &(symbol.inf), BitstreamLengthInBits, symbol.len)) < 0)
    {
        return -1;
    }
    symbol.value1 = symbol.inf;
    bitstream->frame_bitoffset += symbol.len; // move bitstream pointer

    *used_bits+=symbol.len;

    return symbol.inf;
}


int DummySink::read_ue_v (const char *tracestring, Bitstream *bitstream, int *used_bits)
{
    SyntaxElement symbol;

    symbol.type = 0;//SE_HEADER;
    symbol.mapping = &DummySink::linfo_ue;   // Mapping rule

    symbol.len =  GetVLCSymbol (bitstream->streamBuffer, bitstream->frame_bitoffset, &(symbol.inf), bitstream->bitstream_length);
    bitstream->frame_bitoffset += symbol.len;
    DummySink::linfo_ue(symbol.len, symbol.inf, &(symbol.value1), &(symbol.value2));

    *used_bits+=symbol.len;
    return symbol.value1;
}

int DummySink::read_se_v (const char *tracestring, Bitstream *bitstream, int *used_bits)
{
    SyntaxElement symbol;

    symbol.type = 0;//SE_HEADER;
    symbol.mapping = &DummySink::linfo_se;   // Mapping rule: signed integer

    symbol.len =  GetVLCSymbol (bitstream->streamBuffer, bitstream->frame_bitoffset, &(symbol.inf), bitstream->bitstream_length);
    bitstream->frame_bitoffset += symbol.len;
    DummySink::linfo_se(symbol.len, symbol.inf, &(symbol.value1), &(symbol.value2));

    *used_bits+=symbol.len;
    return symbol.value1;
}

typedef enum {
    FREXT_CAVLC444 = 44,       //!< YUV 4:4:4/14 "CAVLC 4:4:4"
    BASELINE       = 66,       //!< YUV 4:2:0/8  "Baseline"
    MAIN           = 77,       //!< YUV 4:2:0/8  "Main"
    EXTENDED       = 88,       //!< YUV 4:2:0/8  "Extended"
    FREXT_HP       = 100,      //!< YUV 4:2:0/8  "High"
    FREXT_Hi10P    = 110,      //!< YUV 4:2:0/10 "High 10"
    FREXT_Hi422    = 122,      //!< YUV 4:2:2/10 "High 4:2:2"
    FREXT_Hi444    = 244,      //!< YUV 4:4:4/14 "High 4:4:4"
    MULTIVIEW_HIGH = 118,      //!< YUV 4:2:0/8  "Multiview High"
    STEREO_HIGH    = 128       //!< YUV 4:2:0/8  "Stereo High"
} ProfileIDC;

static const unsigned char ZZ_SCAN[16]  =
{  0,  1,  4,  8,  5,  2,  3,  6,  9, 12, 13, 10,  7, 11, 14, 15
};

static const unsigned char ZZ_SCAN8[64] =
{  0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

void DummySink::Scaling_List(int sizeOfScalingList, Bitstream *s, int *UsedBits)
{
    int       ScalingList[64];

    int j, scanj;
    int delta_scale, lastScale, nextScale;

    lastScale      = 8;
    nextScale      = 8;

    for(j=0; j<sizeOfScalingList; j++)
    {
        scanj = (sizeOfScalingList==16) ? ZZ_SCAN[j]:ZZ_SCAN8[j];
        if(nextScale!=0)
        {
            delta_scale = read_se_v (   "   : delta_sl   "                           , s, UsedBits);
            nextScale = (lastScale + delta_scale + 256) % 256;
            
        }
        ScalingList[scanj] = (nextScale==0) ? lastScale:nextScale;
        lastScale = ScalingList[scanj];
    }

}

int DummySink::ProcessH264SPS(unsigned char* buffer, int length)
{
    unsigned i;
    unsigned n_ScalingList;
    int reserved_zero;
    int UsedBits = 0;
    int profile_idc, chroma_format_idc, seq_scaling_matrix_present_flag;
    int ScalingList, seq_scaling_list_present_flag, pic_order_cnt_type, num_ref_frames_in_pic_order_cnt_cycle;
    bool frame_mbs_only_flag, frame_cropping_flag;

    Bitstream *sps_bitstream = new Bitstream;
    if(sps_bitstream==NULL){
        return -1;
    }
    sps_bitstream->bitstream_length = length;
    sps_bitstream->streamBuffer = buffer;
    sps_bitstream->ei_flag = 0;
    sps_bitstream->read_len = sps_bitstream->frame_bitoffset = 0;

    
    profile_idc = read_u_v  (8, "SPS: profile_idc"                           , sps_bitstream, &UsedBits);
    read_u_v  (1, "SPS: constrained_set0_flag"                 , sps_bitstream, &UsedBits);
    read_u_v  (1, "SPS: constrained_set1_flag"                 , sps_bitstream, &UsedBits);
    read_u_v  (1, "SPS: constrained_set2_flag"                 , sps_bitstream, &UsedBits);
    read_u_v  (1, "SPS: constrained_set3_flag"                 , sps_bitstream, &UsedBits);
    read_u_v  (4, "SPS: reserved_zero_4bits"                   , sps_bitstream, &UsedBits);

    fH264_level_idc = read_u_v  (8, "SPS: level_idc"                             , sps_bitstream, &UsedBits);
    debug_printf("fH264_level_idc = %d", fH264_level_idc);

    read_ue_v ("SPS: seq_parameter_set_id"                     , sps_bitstream, &UsedBits);
    if((profile_idc==FREXT_HP   ) ||
        (profile_idc==FREXT_Hi10P) ||
        (profile_idc==FREXT_Hi422) ||
        (profile_idc==FREXT_Hi444) ||
        (profile_idc==FREXT_CAVLC444) || 
        (profile_idc==STEREO_HIGH))
    {
        chroma_format_idc = read_ue_v ("SPS: chroma_format_idc"                       , sps_bitstream, &UsedBits);
        if(chroma_format_idc == 3)//YUV444
        {
            read_u_v  (1, "SPS: separate_colour_plane_flag"              , sps_bitstream, &UsedBits);
        }
        read_ue_v ("SPS: bit_depth_luma_minus8"                   , sps_bitstream, &UsedBits);
        read_ue_v ("SPS: bit_depth_chroma_minus8"                 , sps_bitstream, &UsedBits);
        read_u_v  (1, "SPS: lossless_qpprime_y_zero_flag"            , sps_bitstream, &UsedBits);
        seq_scaling_matrix_present_flag = read_u_v  (1, "SPS: seq_scaling_matrix_present_flag"       , sps_bitstream, &UsedBits);
        if (seq_scaling_matrix_present_flag)
        {
            ScalingList = (chroma_format_idc != 3) ? 8 : 12;
            for(i=0; i<ScalingList; i++)
            {
                seq_scaling_list_present_flag   = read_u_v  (1, "SPS: seq_scaling_list_present_flag"         , sps_bitstream, &UsedBits);
                if(seq_scaling_list_present_flag)
                {
                    if(i<6)
                    {
                        Scaling_List(16, sps_bitstream, &UsedBits);
                    }                    
                    else
                    {
                        Scaling_List(64, sps_bitstream, &UsedBits);
                    }
                }
            }
        }
    }

    read_ue_v ("SPS: log2_max_frame_num_minus4"                , sps_bitstream, &UsedBits);
    pic_order_cnt_type = read_ue_v ("SPS: pic_order_cnt_type"                       , sps_bitstream, &UsedBits);
    if (pic_order_cnt_type == 0)
    {
        read_ue_v ("SPS: log2_max_pic_order_cnt_lsb_minus4"           , sps_bitstream, &UsedBits);
    }
    else if (pic_order_cnt_type == 1)
    {
        read_u_v  (1, "SPS: delta_pic_order_always_zero_flag"       , sps_bitstream, &UsedBits);
        read_se_v ("SPS: offset_for_non_ref_pic"                 , sps_bitstream, &UsedBits);
        read_se_v ("SPS: offset_for_top_to_bottom_field"         , sps_bitstream, &UsedBits);
        num_ref_frames_in_pic_order_cnt_cycle = read_ue_v ("SPS: num_ref_frames_in_pic_order_cnt_cycle"  , sps_bitstream, &UsedBits);
        for(i=0; i<num_ref_frames_in_pic_order_cnt_cycle; i++)
        {
            read_se_v ("SPS: offset_for_ref_frame[i]"              , sps_bitstream, &UsedBits);
        }
    }
    read_ue_v ("SPS: num_ref_frames"                         , sps_bitstream, &UsedBits);
    read_u_v  (1, "SPS: gaps_in_frame_num_value_allowed_flag"   , sps_bitstream, &UsedBits);
    fH264_width = (read_ue_v ("SPS: pic_width_in_mbs_minus1"                , sps_bitstream, &UsedBits)+1)*16;
    fH264_height = (read_ue_v ("SPS: pic_height_in_map_units_minus1"         , sps_bitstream, &UsedBits)+1)*16;

    frame_mbs_only_flag                   = read_u_v  (1, "SPS: frame_mbs_only_flag"                    , sps_bitstream, &UsedBits);
    if (!frame_mbs_only_flag)
    {
        read_u_v  (1, "SPS: mb_adaptive_frame_field_flag"           , sps_bitstream, &UsedBits);
    }
    read_u_v  (1, "SPS: direct_8x8_inference_flag"              , sps_bitstream, &UsedBits);
    frame_cropping_flag                   = read_u_v  (1, "SPS: frame_cropping_flag"                    , sps_bitstream, &UsedBits);

    if (frame_cropping_flag)
    {
        read_ue_v ("SPS: frame_crop_left_offset"           , sps_bitstream, &UsedBits);
        read_ue_v ("SPS: frame_crop_right_offset"          , sps_bitstream, &UsedBits);
        read_ue_v ("SPS: frame_crop_top_offset"            , sps_bitstream, &UsedBits);
        read_ue_v ("SPS: frame_crop_bottom_offset"         , sps_bitstream, &UsedBits);
    }


    if (KILL_VUI == true)
    {
        //debug_printf("UsedBits = %d", UsedBits);
        bool erase = true;
        if (erase) {
            if (1 == read_u_v (1, "SPS: vui_parameters_present_flag", sps_bitstream, &UsedBits)) {
                if (255 == read_u_v (8, "VUI: aspect_ratio_idc", sps_bitstream, &UsedBits)) {
                    read_u_v  (16, "VUI: sar_width"                     , sps_bitstream, &UsedBits);
                    read_u_v  (16, "VUI: sar_height"                    , sps_bitstream, &UsedBits);
                }
                write_u_v ("SPS: VUI: overscan_info_present_flag set to 0"        , sps_bitstream, &UsedBits, 1, 0);
                write_u_v ("SPS: VUI: video_signal_type_present_flag set to 0"    , sps_bitstream, &UsedBits, 1, 0);
                write_u_v ("SPS: VUI: chroma_location_info_present_flag set to 0" , sps_bitstream, &UsedBits, 1, 0);
                write_u_v ("SPS: VUI: timing_info_present_flag set to 0"          , sps_bitstream, &UsedBits, 1, 0);
                write_u_v ("SPS: VUI: nal_hrd_parameters_present_flag set to 0"   , sps_bitstream, &UsedBits, 1, 0);
                write_u_v ("SPS: VUI: vcl_hrd_parameters_present_flag set to 0"   , sps_bitstream, &UsedBits, 1, 0);
                write_u_v ("SPS: VUI: pic_struct_present_flag set to 0"           , sps_bitstream, &UsedBits, 1, 0);
                write_u_v ("SPS: VUI: bitstream_restriction_flag set to 0"        , sps_bitstream, &UsedBits, 1, 0);
            }
        } else {
            write_u_v("SPS: vui_parameters_present_flag ==> set to 0"         , sps_bitstream, &UsedBits, 1, 0);
        }
        //debug_printf("UsedBits = %d", UsedBits);
        while (UsedBits & 0x07) {
            write_u_v ("SPS: set remain bits to 0"         , sps_bitstream, &UsedBits, 1, 0);
            //debug_printf("UsedBits = %d", UsedBits);
        }
        return UsedBits / 8;
    }
    else
    {
        read_u_v (1, "SPS: vui_parameters_present_flag"      , sps_bitstream, &UsedBits);
        return length;
    }
}
// If you don't want to see debugging output for each received frame, then comment out the following line:
#define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
                  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  // We've just received a frame of data.  (Optionally) print out information about it:
  int idx = -1;
  jobject java_object = NULL;
  unsigned char *pout = NULL;
  int AudioCodec = AUDIO_UNSUPPORT;
  ourRTSPClient* rtspClient = (ourRTSPClient*)(fSubsession.miscPtr);
  if (!bJniMethodInited) {
	  this->initJni(rtspClient->jenv, rtspClient->jobject_rtspcodec);
  }
  long i_pts = (long)presentationTime.tv_sec*1000000LL + (long)presentationTime.tv_usec;
  /* XXX Beurk beurk beurk Avoid having negative value XXX */
  i_pts &=(0x00ffffffffffffffLL);

  if (strcmp(fSubsession.codecName(), "H264") == 0)
  {
      int nal_unit = fReceiveBuffer[0] & 0x1F;

      if (fNeedToSetVideoMediaCodec)
      {
          if (nal_unit == 7) // sps
          {
              //debug_printf("sps");
              int usableBytes = ProcessH264SPS(fReceiveBuffer+1, frameSize-1);
              if (usableBytes > 0)
              {
#if RECREATE_SPSPPS_BUFFERS
                  jclass jc = jenv->FindClass("java/nio/ByteBuffer");
                  jmethodID jm = jenv->GetStaticMethodID(jc, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
                  jobject jo = jenv->CallStaticObjectMethod(jc, jm, usableBytes+4);
                  pout = (unsigned char *)jenv->GetDirectBufferAddress(jo);
                  pout[0] = 0x00;
                  pout[1] = 0x00;
                  pout[2] = 0x00;
                  pout[3] = 0x01;
                  memcpy(&pout[4], fReceiveBuffer, usableBytes);
                  jenv->DeleteLocalRef(jc);

                  jenv->SetObjectField(jobject_rtspcodec, fieldID_spsBuffer, jo);
#else
                  java_object = jenv->CallObjectMethod(jobject_rtspcodec, methodID_SPSDirectBuffer);
                  pout = (unsigned char *)jenv->GetDirectBufferAddress(java_object);
                  pout[0] = 0x00;
                  pout[1] = 0x00;
                  pout[2] = 0x00;
                  pout[3] = 0x01;
                  memcpy(&pout[4], fReceiveBuffer, frameSize);

                  jclass jc = jenv->FindClass("java/nio/ByteBuffer");
                  jmethodID jm2 = jenv->GetMethodID(jc, "limit", "(I)Ljava/nio/Buffer;");
                  jenv->CallObjectMethod(java_object, jm2, frameSize+4);
                  jenv->DeleteLocalRef(jc);
#endif
                  debug_printf("fH264_width = %d, fH264_height = %d", fH264_width, fH264_height);
              }
          }
          else if (nal_unit == 8) // pps
          {
              //debug_printf("pps %dx%d", fSubsession.videoWidth(), fSubsession.videoHeight());
#if RECREATE_SPSPPS_BUFFERS
              jclass jc = jenv->FindClass("java/nio/ByteBuffer");
              jmethodID jm = jenv->GetStaticMethodID(jc, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
              jobject jo = jenv->CallStaticObjectMethod(jc, jm, frameSize+4);
              pout = (unsigned char *)jenv->GetDirectBufferAddress(jo);
              pout[0] = 0x00;
              pout[1] = 0x00;
              pout[2] = 0x00;
              pout[3] = 0x01;
              memcpy(&pout[4], fReceiveBuffer, frameSize);
              jenv->DeleteLocalRef(jc);

              jenv->SetObjectField(jobject_rtspcodec, fieldID_ppsBuffer, jo);
#else
              java_object = jenv->CallObjectMethod(jobject_rtspcodec, methodID_PPSDirectBuffer);
              pout = (unsigned char *)jenv->GetDirectBufferAddress(java_object);
              pout[0] = 0x00;
              pout[1] = 0x00;
              pout[2] = 0x00;
              pout[3] = 0x01;
              memcpy(&pout[4], fReceiveBuffer, frameSize);

              jclass jc = jenv->FindClass("java/nio/ByteBuffer");
              jmethodID jm2 = jenv->GetMethodID(jc, "limit", "(I)Ljava/nio/Buffer;");
              jenv->CallObjectMethod(java_object, jm2, frameSize+4);
              jenv->DeleteLocalRef(jc);
#endif

              fNeedToSetVideoMediaCodec = 0;
              jenv->CallVoidMethod(jobject_rtspcodec, methodID_SetVideoMediaCodec, fH264_width, fH264_height);
          }
          else
          {
              debug_printf("Wait to receive the first SPS/PPS package");
          }
      }
      else
      {
          if (nal_unit == 5 && fReceivedIDR < 10) 
          { // IPCam will send bad data in the first GOP and make garbage video. The 10 GOP should be enough.s
              fReceivedIDR++;
          }
          if ((nal_unit == 1 || nal_unit == 5) && fReceivedIDR >=  2)
          {
              static int picNo = 0;
              picNo++;
              //debug_printf("picNo = %d", picNo);

              #ifdef ADD_QUEUE_VERSION
                //debug_printf("pciNo %d pts %lld , frameLen %u ",picNo,i_pts/1000LL, frameSize + 4);
                java_object = jenv->CallObjectMethod(jobject_rtspcodec, methodID_GetDirectInputVideoBuffer,(int)(frameSize + 4),i_pts);

                if(java_object != NULL){
                    
                    pout = (unsigned char *)jenv->GetDirectBufferAddress(java_object);
                
                    pout[0] = 0x00;
                    pout[1] = 0x00;
                    pout[2] = 0x00;
                    pout[3] = 0x01;

                    memcpy(&pout[4], fReceiveBuffer, frameSize);
                    
                    //debug_printf("pciNo %d ptsMs %lld=====2 ",picNo,i_pts/1000LL);
                    jenv->CallVoidMethod(jobject_rtspcodec, methodID_FillInputVideoBufferDone, (int) (frameSize+4), i_pts);
                }

                jenv->DeleteLocalRef(java_object);
              #else
              idx = (int)jenv->CallIntMethod(jobject_rtspcodec, methodID_AskDequeueVideoInputBufferIdx);


              if (idx < INBUF_CNT && idx >= 0)
              {
                  if (pvVideoDirectBufferFromJava[idx] == NULL)
                  {
                      java_object = jenv->CallObjectMethod(jobject_rtspcodec, methodID_InputVideoDirectBuffer);
                      pvVideoDirectBufferFromJava[idx] = (unsigned char *)jenv->GetDirectBufferAddress(java_object);
                  }

                  pout = pvVideoDirectBufferFromJava[idx];
                  pout[0] = 0x00;
                  pout[1] = 0x00;
                  pout[2] = 0x00;
                  pout[3] = 0x01;
                  memcpy(&pout[4], fReceiveBuffer, frameSize);

                  jenv->CallVoidMethod(jobject_rtspcodec, methodID_vdecode, frameSize+4, i_pts);

              }  

              #endif
          }            
      }
  }
  else if (strcmp(fSubsession.mediumName(), "audio") == 0) //aac 
  {
      if (strcmp(fSubsession.codecName(), "MPEG4-GENERIC") == 0) //aac 
      {
          AudioCodec = AAC;
      }
      else if (strcmp(fSubsession.codecName(), "PCMU") == 0) //G.711 ulaw 
      {
          AudioCodec = PCMU;
      }
      else if (strcmp(fSubsession.codecName(), "PCMA") == 0) //G.711 alaw
      {
          AudioCodec = PCMA;
      }
      else
      {
          debug_printf("%s not support yet", fSubsession.codecName());
      }

 
      if (AudioCodec != AUDIO_UNSUPPORT)
      {
          if (fNeedToSetAudioMediaCodec)
          {
              unsigned int configLen;
              unsigned char* configData = parseGeneralConfigStr(fSubsession.fmtp_config(), configLen);

              java_object = jenv->CallObjectMethod(jobject_rtspcodec, methodID_ESDSDirectBuffer);
              pout = (unsigned char *)jenv->GetDirectBufferAddress(java_object);

              memcpy(pout, configData, configLen);

              delete[] configData;

              jenv->CallVoidMethod(jobject_rtspcodec, methodID_SetAudioMediaCodec,
                  fSubsession.numChannels(), fSubsession.rtpTimestampFrequency(), AudioCodec);
              fNeedToSetAudioMediaCodec = 0;           
          }


          idx = (int)jenv->CallIntMethod(jobject_rtspcodec, methodID_AskDequeueAudioInputBufferIdx);

          if (idx < INBUF_CNT && idx >= 0)
          {
              if (pvAudioDirectBufferFromJava[idx] == NULL)
              {
                  java_object = jenv->CallObjectMethod(jobject_rtspcodec, methodID_InputAudioDirectBuffer);
                  pvAudioDirectBufferFromJava[idx] = (unsigned char *)jenv->GetDirectBufferAddress(java_object);
              }

              pout = pvAudioDirectBufferFromJava[idx];
              memcpy(pout, fReceiveBuffer, frameSize);
              jenv->CallVoidMethod(jobject_rtspcodec, methodID_adecode, frameSize, i_pts);
          }  
      }    
  }



#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
  if (fStreamId != NULL) envir() << "Stream \"" << fStreamId << "\"; ";
  envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";

  if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";

  char uSecsStr[6+1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (int)presentationTime.tv_sec << "." << uSecsStr;
  ////debug_printf("Presentation time: %d", (int)presentationTime.tv_sec);

  if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
    envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
  }
#ifdef DEBUG_PRINT_NPT
  envir() << "\tNPT: " << fSubsession.getNormalPlayTime(presentationTime);
#endif
  envir() << "\n";
#endif

  // Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean DummySink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}

void *thread_rtsp(void * arg)
{
    debug_printf("thread_rtsp coming\n");

    ourRTSPClient *rtspClient = (ourRTSPClient *)arg;
    debug_printf("%s", rtspClient->url());
    rtspClient->eventLoopWatchVariable = 0;

    pthread_detach(pthread_self());
    gs_jvm->AttachCurrentThread(&rtspClient->jenv, NULL);
    rtspClient->sendDescribeCommand(continueAfterDESCRIBE);
    rtspClient->envir().taskScheduler().doEventLoop(&rtspClient->eventLoopWatchVariable);
    shutdownStream(rtspClient);

    //debug_printf("thread_rtsp exit\n");
    gs_jvm->DetachCurrentThread();
    pthread_exit(0);
}

void DummySink::initJni(JNIEnv* env, jobject obj) {
	if (!bJniMethodInited) {
		int i;
		jenv = env;
		jobject_rtspcodec = obj;

		jclass jclass_rtspcodec = jenv->GetObjectClass(jobject_rtspcodec);
		methodID_vdecode = jenv->GetMethodID(jclass_rtspcodec, "VideoDecodeCallback", "(IJ)V");
		if (methodID_vdecode == NULL)
		{
			debug_printf("Error! methodID_vdecode is NULL!\n");
		}
		methodID_SetVideoMediaCodec = jenv->GetMethodID(jclass_rtspcodec, "SetVideoMediaCodecCallback", "(II)V");
		if (methodID_SetVideoMediaCodec == NULL)
		{
			debug_printf("Error! methodID_SetVideoMediaCodec is NULL!\n");
		}
		methodID_InputVideoDirectBuffer = jenv->GetMethodID(jclass_rtspcodec, "InputVideoDirectBufferCallback", "()Ljava/nio/ByteBuffer;");
		if (methodID_InputVideoDirectBuffer == NULL)
		{
			debug_printf("Error! methodID_InputVideoDirectBuffer is NULL!\n");
		}
		methodID_SPSDirectBuffer = jenv->GetMethodID(jclass_rtspcodec, "SPSDirectBufferCallback", "()Ljava/nio/ByteBuffer;");
		if (methodID_SPSDirectBuffer == NULL)
		{
			debug_printf("Error! methodID_SPSDirectBuffer is NULL!\n");
		}
		methodID_PPSDirectBuffer = jenv->GetMethodID(jclass_rtspcodec, "PPSDirectBufferCallback", "()Ljava/nio/ByteBuffer;");
		if (methodID_PPSDirectBuffer == NULL)
		{
			debug_printf("Error! methodID_PPSDirectBuffer is NULL!\n");
		}
		methodID_AskDequeueVideoInputBufferIdx = jenv->GetMethodID(jclass_rtspcodec, "AskDequeueVideoInputBufferIdxCallback", "()I");
		if (methodID_AskDequeueVideoInputBufferIdx == 0)
		{
			debug_printf("Error! methodID_AskDequeueVideoInputBufferIdx is NULL!\n");
		}
		for (i = 0; i < INBUF_CNT; i++)
		{
			pvVideoDirectBufferFromJava [i] = NULL;
		}


        //////////////////////////////////////////////////////
        //add by shinechen
        #ifdef ADD_QUEUE_VERSION
        methodID_GetDirectInputVideoBuffer = jenv->GetMethodID(jclass_rtspcodec, "GetDirectInputVideoBuffer", "(IJ)Ljava/nio/ByteBuffer;");
        if (methodID_GetDirectInputVideoBuffer == NULL)
        {
            debug_printf("Error! methodID_GetDirectInputVideoBuffer is NULL!\n");
        }
        
        //methodID_FillInputVideoBufferDone = jenv->GetMethodID(jclass_rtspcodec, "FillInputVideoBufferDoneCallback", "(Ljava/nio/ByteBuffer;IJ)V");
        methodID_FillInputVideoBufferDone = jenv->GetMethodID(jclass_rtspcodec, "FillInputVideoBufferDoneCallback", "(IJ)V");
        if (methodID_FillInputVideoBufferDone == NULL)
        {
            debug_printf("Error! methodID_FillInputVideoBufferDone is NULL!\n");
        }
        #endif

        //////////////////////////////////////////////////////


		methodID_adecode = jenv->GetMethodID(jclass_rtspcodec, "AudioDecodeCallback", "(IJ)V");
		if (methodID_adecode == NULL)
		{
			debug_printf("Error! methodID_adecode is NULL!\n");
		}
		methodID_InputAudioDirectBuffer = jenv->GetMethodID(jclass_rtspcodec, "InputAudioDirectBufferCallback", "()Ljava/nio/ByteBuffer;");
		if (methodID_InputAudioDirectBuffer == NULL)
		{
			debug_printf("Error! methodID_InputAudioDirectBuffer is NULL!\n");
		}
		methodID_SetAudioMediaCodec = jenv->GetMethodID(jclass_rtspcodec, "SetAudioMediaCodecCallback", "(III)V");
		if (methodID_SetAudioMediaCodec == NULL)
		{
			debug_printf("Error! methodID_SetAudioMediaCodec is NULL!\n");
		}
		methodID_ESDSDirectBuffer = jenv->GetMethodID(jclass_rtspcodec, "ESDSDirectBufferCallback", "()Ljava/nio/ByteBuffer;");
		if (methodID_ESDSDirectBuffer == NULL)
		{
			debug_printf("Error! methodID_ESDSDirectBuffer is NULL!\n");
		}
		methodID_AskDequeueAudioInputBufferIdx = jenv->GetMethodID(jclass_rtspcodec, "AskDequeueAudioInputBufferIdxCallback", "()I");
		if (methodID_AskDequeueAudioInputBufferIdx == 0)
		{
			debug_printf("Error! methodID_AskDequeueAudioInputBufferIdx is NULL!\n");
		}
		for (i = 0; i < INBUF_CNT; i++)
		{
			pvAudioDirectBufferFromJava [i] = NULL;
		}

		fieldID_spsBuffer = jenv->GetFieldID(jclass_rtspcodec, "spsBuffer", "Ljava/nio/ByteBuffer;");
		fieldID_ppsBuffer = jenv->GetFieldID(jclass_rtspcodec, "ppsBuffer", "Ljava/nio/ByteBuffer;");

		jenv->DeleteLocalRef(jclass_rtspcodec);
		bJniMethodInited = true;
  }
}


extern "C"
{
    jint JNI_OnLoad(JavaVM *vm, void * reserved)
    {
        //debug_printf("JNI_OnLoad!!!!!!\n");
        gs_jvm = vm;
        return JNI_VERSION_1_2;
    }

    JNIEXPORT jlong JNICALL LIBBASE_API(nativeStartRTSP, jstring java_rtsp_path)
    {
        const char *rtsp_path = env->GetStringUTFChars(java_rtsp_path, 0);
        pthread_t rtsp_thread_id;
        TaskScheduler* scheduler = BasicTaskScheduler::createNew();
        UsageEnvironment* usage_env = BasicUsageEnvironment::createNew(*scheduler);
        ourRTSPClient* rtspClient = ourRTSPClient::createNew(*usage_env, rtsp_path, RTSP_CLIENT_VERBOSITY_LEVEL, "test");
        rtspClient->jobject_rtspcodec = env->NewGlobalRef(obj);

        debug_printf("JNI start coming: %s\n", rtsp_path);
        env->ReleaseStringUTFChars(java_rtsp_path, rtsp_path);
        pthread_create(&rtsp_thread_id, NULL, thread_rtsp, (void*) rtspClient);
        return (long)rtspClient;
    }

    JNIEXPORT jint JNICALL LIBBASE_API(navtiveStopRTSP, jlong handle)
    {
      ourRTSPClient* rtspClient = (ourRTSPClient*) handle;
      if (rtspClient != NULL && rtspClient->check_structure_code == 0xC0DECD0E) {
        rtspClient->eventLoopWatchVariable = 1;
        return JNI_OK;
      }
        return JNI_ERR;
    }

    int checkURI_Watch;
    int checkURI_Result;
    void checkResult(RTSPClient* rtspClient, int resultCode, char* resultString) {
        checkURI_Watch = 1;
        checkURI_Result = resultCode;
        debug_printf("checkURI : checkResult = %d, %s", resultCode, resultString);
    }

    JNIEXPORT jboolean JNICALL LIBBASE_API(nativeCheckURI, jstring uri)
    {
        checkURI_Watch = 0;
        checkURI_Result = 0;
        if (uri != NULL)
        {
            const char* rtspURL = env->GetStringUTFChars((jstring)uri, NULL);
            if (rtspURL != NULL)
            {
                debug_printf("checkURI : check --> %s", rtspURL);

                TaskScheduler* scheduler = BasicTaskScheduler::createNew();
                UsageEnvironment* usage_env = BasicUsageEnvironment::createNew(*scheduler);
                RTSPClient* rtspClient = ourRTSPClient::createNew(*usage_env, rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, "test");
                if (rtspClient == NULL) {
                    debug_printf("checkURI : rtspClient = NULL! Return.");
                    env->ReleaseStringUTFChars(uri, rtspURL);
                    return JNI_FALSE;
                }
                rtspClient->sendDescribeCommand(checkResult);

                usage_env->taskScheduler().doEventLoop((char*)&checkURI_Watch);
                debug_printf("checkURI : result --> %d", checkURI_Result);
                shutdownStream(rtspClient);
                env->ReleaseStringUTFChars(uri, rtspURL);
                return (checkURI_Result == 0) ? JNI_TRUE : JNI_FALSE;
            }
        }
        return JNI_FALSE;
    }
}
