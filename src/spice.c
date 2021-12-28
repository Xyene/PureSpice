/*
PureSpice - A pure C implementation of the SPICE client protocol
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
https://github.com/gnif/PureSpice

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "spice/spice.h"

#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <assert.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <spice/protocol.h>
#include <spice/vd_agent.h>

#include "locking.h"
#include "messages.h"
#include "rsa.h"
#include "queue.h"

// we don't really need flow control because we are all local
// instead do what the spice-gtk library does and provide the largest
// possible number
#define SPICE_AGENT_TOKENS_MAX ~0

#define _SPICE_RAW_PACKET(htype, dataSize, extraData, _alloc) \
({ \
  uint8_t * packet = _alloc(sizeof(ssize_t) + \
      sizeof(SpiceMiniDataHeader) + dataSize); \
  ssize_t * sz = (ssize_t*)packet; \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(sz + 1); \
  *sz          = sizeof(SpiceMiniDataHeader) + dataSize; \
  header->type = (htype); \
  header->size = dataSize + extraData; \
  (header + 1); \
})

#define SPICE_RAW_PACKET(htype, dataSize, extraData) \
  _SPICE_RAW_PACKET(htype, dataSize, extraData, alloca)

#define SPICE_RAW_PACKET_MALLOC(htype, dataSize, extraData) \
  _SPICE_RAW_PACKET(htype, dataSize, extraData, malloc)

#define SPICE_RAW_PACKET_FREE(packet) \
{ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  free(sz); \
}

#define SPICE_SET_PACKET_SIZE(packet, sz) \
{ \
   SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
   header->size = sz; \
}

#define SPICE_PACKET(htype, payloadType, extraData) \
  ((payloadType *)SPICE_RAW_PACKET(htype, sizeof(payloadType), extraData))

#define SPICE_PACKET_MALLOC(htype, payloadType, extraData) \
  ((payloadType *)SPICE_RAW_PACKET_MALLOC(htype, sizeof(payloadType), extraData))

#define SPICE_SEND_PACKET(channel, packet) \
({ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  SPICE_LOCK((channel)->lock); \
  const ssize_t wrote = send((channel)->socket, header, *sz, 0); \
  SPICE_UNLOCK((channel)->lock); \
  wrote == *sz; \
})

#define SPICE_SEND_PACKET_NL(channel, packet) \
({ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  const ssize_t wrote = send((channel)->socket, header, *sz, 0); \
  wrote == *sz; \
})

// currently (2020) these defines are not yet availble for most distros, so we
// just define them ourselfs for now
#define _SPICE_MOUSE_BUTTON_SIDE        6
#define _SPICE_MOUSE_BUTTON_EXTRA       7
#define _SPICE_MOUSE_BUTTON_MASK_SIDE  (1 << 5)
#define _SPICE_MOUSE_BUTTON_MASK_EXTRA (1 << 6)

typedef enum
{
  PS_STATUS_OK,
  PS_STATUS_HANDLED,
  PS_STATUS_NODATA,
  PS_STATUS_ERROR
}
PS_STATUS;

// internal structures
struct PSChannel
{
  bool        connected;
  bool        ready;
  bool        initDone;
  uint8_t     channelType;
  int         socket;
  uint32_t    ackFrequency;
  uint32_t    ackCount;
  atomic_flag lock;

  PS_STATUS (*read)(int * dataAvailable);
};

struct PS
{
  char         password[32];
  short        family;
  union
  {
    struct sockaddr     addr;
    struct sockaddr_in  in;
    struct sockaddr_in6 in6;
    struct sockaddr_un  un;
  }
  addr;

  bool     hasAgent;
  _Atomic(uint32_t) serverTokens;
  uint32_t sessionID;
  uint32_t channelID;
  ssize_t  agentMsg;

  int      epollfd;
  struct   PSChannel scMain;
  struct   PSChannel scInputs;
  struct   PSChannel scPlayback;

  struct
  {
    uint32_t modifiers;
  }
  kb;

  struct
  {
    atomic_flag lock;
    uint32_t buttonState;

    atomic_int sentCount;
    int rpos, wpos;
  }
  mouse;

  bool cbSupported;
  bool cbSelection;

  // clipboard variables
  bool                  cbAgentGrabbed;
  bool                  cbClientGrabbed;
  PSDataType         cbType;
  uint8_t *             cbBuffer;
  uint32_t              cbRemain;
  uint32_t              cbSize;
  PSClipboardNotice  cbNoticeFn;
  PSClipboardData    cbDataFn;
  PSClipboardRelease cbReleaseFn;
  PSClipboardRequest cbRequestFn;

  uint8_t * motionBuffer;
  size_t    motionBufferSize;

  struct Queue * agentQueue;

  bool playback;
  void (*playbackStart)(int channels, int sampleRate, PSAudioFormat format,
    uint32_t time);
  void (*playbackVolume)(int channels, const uint16_t volume[]);
  void (*playbackMute)(bool mute);
  void (*playbackStop)(void);
  void (*playbackData)(uint8_t * data, size_t size);
};

static PS_STATUS purespice_onCommonRead(struct PSChannel * channel,
    SpiceMiniDataHeader * header, int * dataAvailable);

static PS_STATUS purespice_onMainChannelRead(int * dataAvailable);

static PS_STATUS purespice_onInputsChannelRead(int * dataAvailable);

static PS_STATUS purespice_onPlaybackChannelRead(int * dataAvailable);

// globals
struct PS spice =
{
  .scMain    .channelType = SPICE_CHANNEL_MAIN,
  .scMain    .read        = purespice_onMainChannelRead,
  .scInputs  .channelType = SPICE_CHANNEL_INPUTS,
  .scInputs  .read        = purespice_onInputsChannelRead,
  .scPlayback.channelType = SPICE_CHANNEL_PLAYBACK,
  .scPlayback.read        = purespice_onPlaybackChannelRead
};

// internal forward decls
static PS_STATUS purespice_connectChannel(struct PSChannel * channel);
static void         purespice_disconnectChannel(struct PSChannel * channel);

static bool purespice_processAck(struct PSChannel * channel);

static PS_STATUS purespice_agentProcess(uint32_t dataSize, int * dataAvailable);
static bool         purespice_agentProcessQueue(void);
static PS_STATUS purespice_agentConnect();
static PS_STATUS purespice_agentSendCaps(bool request);
static void         purespice_agentOnClipboard();

// utility functions
static uint32_t purespice_typeToAgentType(PSDataType type);
static PSDataType agent_type_to_purespice_type(uint32_t type);

// thread safe read/write methods
static bool purespice_agentStartMsg(uint32_t type, ssize_t size);
static bool purespice_agentWriteMsg(const void * buffer, ssize_t size);

// non thread safe read/write methods (nl = non-locking)
static PS_STATUS purespice_readNL(struct PSChannel * channel, void * buffer,
    const ssize_t size, int * dataAvailable);

static PS_STATUS purespice_discardNL(struct PSChannel * channel, ssize_t size,
    int * dataAvailable);

static ssize_t purespice_writeNL(const struct PSChannel * channel,
    const void * buffer, const ssize_t size);

static uint64_t get_timestamp()
{
  struct timespec time;
  const int result = clock_gettime(CLOCK_MONOTONIC, &time);
  if (result != 0)
    perror("clock_gettime failed! this should never happen!\n");
  return (uint64_t)time.tv_sec * 1000LL + time.tv_nsec / 1000000LL;
}

bool purespice_connect(const char * host, const unsigned short port,
    const char * password, bool playback)
{
  strncpy(spice.password, password, sizeof(spice.password) - 1);
  memset(&spice.addr, 0, sizeof(spice.addr));
  spice.playback = playback;

  if (port == 0)
  {
    spice.family = AF_UNIX;
    spice.addr.un.sun_family = spice.family;
    strncpy(spice.addr.un.sun_path, host, sizeof(spice.addr.un.sun_path) - 1);
  }
  else
  {
    spice.family = AF_INET;
    inet_pton(spice.family, host, &spice.addr.in.sin_addr);
    spice.addr.in.sin_family = spice.family;
    spice.addr.in.sin_port   = htons(port);
  }

  spice.epollfd = epoll_create1(0);
  if (spice.epollfd < 0)
    perror("epoll_create1 failed!\n");

  spice.channelID = 0;
  if (purespice_connectChannel(&spice.scMain) != PS_STATUS_OK)
  {
    close(spice.epollfd);
    return false;
  }

  return true;
}

void purespice_disconnect()
{
  purespice_disconnectChannel(&spice.scInputs);
  purespice_disconnectChannel(&spice.scMain  );
  close(spice.epollfd);

  if (spice.motionBuffer)
  {
    free(spice.motionBuffer);
    spice.motionBuffer = NULL;
  }

  if (spice.agentQueue)
  {
    void * msg;
    while(queue_shift(spice.agentQueue, &msg))
      SPICE_RAW_PACKET_FREE(msg);
    queue_free(spice.agentQueue);
    spice.agentQueue = NULL;
  }

  spice.hasAgent = false;
}

bool purespice_ready()
{
  return spice.scMain.connected &&
         spice.scInputs.connected;
}

bool purespice_process(int timeout)
{
  #define MAX_EVENTS 4
  static struct epoll_event events[MAX_EVENTS];

  int nfds = epoll_wait(spice.epollfd, events, MAX_EVENTS, timeout);
  if (nfds == 0)
    return true;

  if (nfds < 0)
    return false;

  for(int i = 0; i < nfds; ++i)
  {
    struct PSChannel * channel = (struct PSChannel *)events[i].data.ptr;

    int dataAvailable;
    ioctl(channel->socket, FIONREAD, &dataAvailable);

    if (!dataAvailable)
      channel->connected = false;
    else
      while(dataAvailable > 0)
      {
        switch(channel->read(&dataAvailable))
        {
          case PS_STATUS_OK:
          case PS_STATUS_HANDLED:
            // if dataAvailable has gone negative then refresh it
            if (dataAvailable < 0)
              ioctl(channel->socket, FIONREAD, &dataAvailable);
            break;

          case PS_STATUS_NODATA:
            channel->connected = false;
            close(channel->socket);
            dataAvailable = 0;
            break;

          default:
            return false;
        }

        if (channel->connected && !purespice_processAck(channel))
          return false;
      }
  }

  if (spice.scMain.connected || spice.scInputs.connected)
    return true;

  /* shutdown */
  spice.sessionID = 0;
  if (spice.cbBuffer)
  {
    free(spice.cbBuffer);
    spice.cbBuffer = NULL;
  }

  spice.cbRemain = 0;
  spice.cbSize   = 0;

  spice.cbAgentGrabbed  = false;
  spice.cbClientGrabbed = false;

  if (spice.scInputs.connected)
    close(spice.scInputs.socket);

  if (spice.scMain.connected)
    close(spice.scMain.socket);

  return false;
}

static bool purespice_processAck(struct PSChannel * channel)
{
  if (channel->ackFrequency == 0)
    return true;

  if (channel->ackCount++ != channel->ackFrequency)
    return true;

  channel->ackCount = 0;

  char * ack = SPICE_PACKET(SPICE_MSGC_ACK, char, 0);
  *ack = 0;
  return SPICE_SEND_PACKET(channel, ack);
}

static PS_STATUS purespice_onCommonRead(struct PSChannel * channel,
    SpiceMiniDataHeader * header, int * dataAvailable)
{
  PS_STATUS status;
  if ((status = purespice_readNL(channel, header, sizeof(SpiceMiniDataHeader),
          dataAvailable)) != PS_STATUS_OK)
    return status;

  if (!channel->connected)
    return PS_STATUS_HANDLED;

  if (!channel->initDone)
    return PS_STATUS_OK;

  switch(header->type)
  {
    case SPICE_MSG_MIGRATE:
    case SPICE_MSG_MIGRATE_DATA:
      return PS_STATUS_HANDLED;

    case SPICE_MSG_SET_ACK:
    {
      SpiceMsgSetAck in;
      if ((status = purespice_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      channel->ackFrequency = in.window;

      SpiceMsgcAckSync * out =
        SPICE_PACKET(SPICE_MSGC_ACK_SYNC, SpiceMsgcAckSync, 0);

      out->generation = in.generation;
      return SPICE_SEND_PACKET(channel, out) ?
        PS_STATUS_HANDLED : PS_STATUS_ERROR;
    }

    case SPICE_MSG_PING:
    {
      SpiceMsgPing in;
      if ((status = purespice_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      const int discard = header->size - sizeof(in);
      if ((status = purespice_discardNL(channel, discard,
              dataAvailable)) != PS_STATUS_OK)
        return status;

      SpiceMsgcPong * out =
        SPICE_PACKET(SPICE_MSGC_PONG, SpiceMsgcPong, 0);

      out->id        = in.id;
      out->timestamp = in.timestamp;
      return SPICE_SEND_PACKET(channel, out) ?
        PS_STATUS_HANDLED : PS_STATUS_ERROR;
    }

    case SPICE_MSG_WAIT_FOR_CHANNELS:
      return PS_STATUS_HANDLED;

    case SPICE_MSG_DISCONNECTING:
    {
      shutdown(channel->socket, SHUT_WR);
      return PS_STATUS_HANDLED;
    }

    case SPICE_MSG_NOTIFY:
    {
      SpiceMsgNotify * in = (SpiceMsgNotify *)alloca(header->size);
      if ((status = purespice_readNL(channel, in, header->size,
              dataAvailable)) != PS_STATUS_OK)
        return status;

      //TODO: send this to a logging function/interface

      return PS_STATUS_HANDLED;
    }
  }

  return PS_STATUS_OK;
}

static PS_STATUS purespice_onMainChannelRead(int * dataAvailable)
{
  struct PSChannel *channel = &spice.scMain;

  SpiceMiniDataHeader header;

  PS_STATUS status;
  if ((status = purespice_onCommonRead(channel, &header,
          dataAvailable)) != PS_STATUS_OK)
    return status;

  if (!channel->initDone)
  {
    if (header.type != SPICE_MSG_MAIN_INIT)
    {
      purespice_disconnect();
      return PS_STATUS_ERROR;
    }

    channel->initDone = true;
    SpiceMsgMainInit msg;
    if ((status = purespice_readNL(channel, &msg, sizeof(msg),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    spice.sessionID = msg.session_id;

    atomic_store(&spice.serverTokens, msg.agent_tokens);
    if (msg.agent_connected && (status = purespice_agentConnect()) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    if (msg.current_mouse_mode != SPICE_MOUSE_MODE_CLIENT &&
        !purespice_mouseMode(false))
      return PS_STATUS_ERROR;

    void * packet = SPICE_RAW_PACKET(SPICE_MSGC_MAIN_ATTACH_CHANNELS, 0, 0);
    if (!SPICE_SEND_PACKET(channel, packet))
    {
      purespice_disconnect();
      return PS_STATUS_ERROR;
    }

    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_CHANNELS_LIST)
  {
    SpiceMainChannelsList *msg = (SpiceMainChannelsList*)alloca(header.size);
    if ((status = purespice_readNL(channel, msg, header.size,
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    for(int i = 0; i < msg->num_of_channels; ++i)
    {
      switch(msg->channels[i].type)
      {
        case SPICE_CHANNEL_INPUTS:
          if (spice.scInputs.connected)
          {
            purespice_disconnect();
            return PS_STATUS_ERROR;
          }

          if ((status = purespice_connectChannel(&spice.scInputs))
              != PS_STATUS_OK)
          {
            purespice_disconnect();
            return status;
          }

          if (spice.scPlayback.connected)
            return PS_STATUS_OK;
          break;

        case SPICE_CHANNEL_PLAYBACK:
          if (!spice.playback)
            break;

          if (spice.scPlayback.connected)
          {
            purespice_disconnect();
            return PS_STATUS_ERROR;
          }

          if ((status = purespice_connectChannel(&spice.scPlayback))
              != PS_STATUS_OK)
          {
            purespice_disconnect();
            return status;
          }

          if (spice.scInputs.connected)
            return PS_STATUS_OK;
          break;
      }
    }

    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED)
  {
    if ((status = purespice_agentConnect()) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }
    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS)
  {
    uint32_t num_tokens;
    if ((status = purespice_readNL(channel, &num_tokens, sizeof(num_tokens),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    atomic_store(&spice.serverTokens, num_tokens);
    if ((status = purespice_agentConnect()) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }
    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DISCONNECTED)
  {
    uint32_t error;
    if ((status = purespice_readNL(channel, &error, sizeof(error),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    spice.hasAgent = false;

    if (spice.cbBuffer)
    {
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbSize   = 0;
      spice.cbRemain = 0;
    }

    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DATA)
  {
    if (!spice.hasAgent)
      return purespice_discardNL(channel, header.size, dataAvailable);

    if ((status = purespice_agentProcess(header.size,
            dataAvailable)) != PS_STATUS_OK)
      purespice_disconnect();

    return status;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_TOKEN)
  {
    uint32_t num_tokens;
    if ((status = purespice_readNL(channel, &num_tokens, sizeof(num_tokens),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    atomic_fetch_add(&spice.serverTokens, num_tokens);
    if (!purespice_agentProcessQueue())
    {
      purespice_disconnect();
      return PS_STATUS_ERROR;
    }

    return PS_STATUS_OK;
  }

  return purespice_discardNL(channel, header.size, dataAvailable);
}

static PS_STATUS purespice_onInputsChannelRead(int * dataAvailable)
{
  struct PSChannel *channel = &spice.scInputs;

  SpiceMiniDataHeader header;

  PS_STATUS status;
  if ((status = purespice_onCommonRead(channel, &header,
          dataAvailable)) != PS_STATUS_OK)
    return status;

  switch(header.type)
  {
    case SPICE_MSG_INPUTS_INIT:
    {
      if (channel->initDone)
        return PS_STATUS_ERROR;

      channel->initDone = true;

      SpiceMsgInputsInit in;
      if ((status = purespice_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      return PS_STATUS_OK;
    }

    case SPICE_MSG_INPUTS_KEY_MODIFIERS:
    {
      SpiceMsgInputsInit in;
      if ((status = purespice_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      spice.kb.modifiers = in.modifiers;
      return PS_STATUS_OK;
    }

    case SPICE_MSG_INPUTS_MOUSE_MOTION_ACK:
    {
      const int count = atomic_fetch_sub(&spice.mouse.sentCount,
          SPICE_INPUT_MOTION_ACK_BUNCH);
      return (count >= SPICE_INPUT_MOTION_ACK_BUNCH) ?
        PS_STATUS_OK : PS_STATUS_ERROR;
    }
  }

  return purespice_discardNL(channel, header.size, dataAvailable);
}

static PS_STATUS purespice_onPlaybackChannelRead(int * dataAvailable)
{
  struct PSChannel *channel = &spice.scPlayback;

  SpiceMiniDataHeader header;

  PS_STATUS status;
  if ((status = purespice_onCommonRead(channel, &header,
          dataAvailable)) != PS_STATUS_OK)
    return status;

  switch(header.type)
  {
    case SPICE_MSG_PLAYBACK_START:
    {
      SpiceMsgPlaybackStart in;
      if ((status = purespice_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (spice.playbackStart)
      {
        PSAudioFormat fmt = PS_AUDIO_FMT_INVALID;
        if (in.format == SPICE_AUDIO_FMT_S16)
          fmt = PS_AUDIO_FMT_S16;

        spice.playbackStart(in.channels, in.frequency, fmt, in.time);
      }
      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_DATA:
    {
      SpiceMsgPlaybackPacket * in =
        (SpiceMsgPlaybackPacket *)alloca(header.size);
      if ((status = purespice_readNL(channel, in, header.size,
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (spice.playbackData)
        spice.playbackData(in->data, header.size - sizeof(*in));

      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_STOP:
      if (spice.playbackStop)
        spice.playbackStop();
      return PS_STATUS_OK;

    case SPICE_MSG_PLAYBACK_VOLUME:
    {
      SpiceMsgAudioVolume * in =
        (SpiceMsgAudioVolume *)alloca(header.size);
      if ((status = purespice_readNL(channel, in, header.size,
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (spice.playbackVolume)
        spice.playbackVolume(in->nchannels, in->volume);

      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_MUTE:
    {
      SpiceMsgAudioMute in;
      if ((status = purespice_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (spice.playbackMute)
        spice.playbackMute(in.mute);

      return PS_STATUS_OK;
    }
  }

  return purespice_discardNL(channel, header.size, dataAvailable);
}

static PS_STATUS purespice_connectChannel(struct PSChannel * channel)
{
  PS_STATUS status;

  channel->initDone     = false;
  channel->ackFrequency = 0;
  channel->ackCount     = 0;

  if (channel == &spice.scInputs)
    SPICE_LOCK_INIT(spice.mouse.lock);

  SPICE_LOCK_INIT(channel->lock);

  size_t addrSize;
  switch(spice.family)
  {
    case AF_UNIX:
      addrSize = sizeof(spice.addr.un);
      break;

    case AF_INET:
      addrSize = sizeof(spice.addr.in);
      break;

    case AF_INET6:
      addrSize = sizeof(spice.addr.in6);
      break;

    default:
      return PS_STATUS_ERROR;
  }

  channel->socket = socket(spice.family, SOCK_STREAM, 0);
  if (channel->socket == -1)
    return PS_STATUS_ERROR;

  if (spice.family != AF_UNIX)
  {
    const int flag = 1;
    setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY , &flag, sizeof(int));
    setsockopt(channel->socket, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(int));
  }

  if (connect(channel->socket, &spice.addr.addr, addrSize) == -1)
  {
    close(channel->socket);
    return PS_STATUS_ERROR;
  }

  channel->connected = true;

  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[MAIN_CAPS_BYTES   / sizeof(uint32_t)];
  }
  __attribute__((packed)) ConnectPacket;

  ConnectPacket p =
  {
    .header = {
      .magic         = SPICE_MAGIC        ,
      .major_version = SPICE_VERSION_MAJOR,
      .minor_version = SPICE_VERSION_MINOR,
      .size          = sizeof(ConnectPacket) - sizeof(SpiceLinkHeader)
    },
    .message = {
      .connection_id    = spice.sessionID,
      .channel_type     = channel->channelType,
      .channel_id       = spice.channelID,
      .num_common_caps  = COMMON_CAPS_BYTES / sizeof(uint32_t),
      .num_channel_caps = MAIN_CAPS_BYTES   / sizeof(uint32_t),
      .caps_offset      = sizeof(SpiceLinkMess)
    }
  };

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  if (channel == &spice.scMain)
    MAIN_SET_CAPABILITY(p.channelCaps, SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS);

  if (channel == &spice.scPlayback)
    PLAYBACK_SET_CAPABILITY(p.channelCaps, SPICE_PLAYBACK_CAP_VOLUME);

  if (purespice_writeNL(channel, &p, sizeof(p)) != sizeof(p))
  {
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  if ((status = purespice_readNL(channel, &p.header, sizeof(p.header),
          NULL)) != PS_STATUS_OK)
  {
    purespice_disconnectChannel(channel);
    return status;
  }

  if (p.header.magic         != SPICE_MAGIC ||
      p.header.major_version != SPICE_VERSION_MAJOR)
  {
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  if (p.header.size < sizeof(SpiceLinkReply))
  {
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  SpiceLinkReply reply;
  if ((status = purespice_readNL(channel, &reply, sizeof(reply),
          NULL)) != PS_STATUS_OK)
  {
    purespice_disconnectChannel(channel);
    return status;
  }

  if (reply.error != SPICE_LINK_ERR_OK)
  {
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  uint32_t capsCommon [reply.num_common_caps ];
  uint32_t capsChannel[reply.num_channel_caps];
  if ((status = purespice_readNL(channel,
          &capsCommon , sizeof(capsCommon ), NULL)) != PS_STATUS_OK ||
      (status = purespice_readNL(channel,
          &capsChannel, sizeof(capsChannel), NULL)) != PS_STATUS_OK)
  {
    purespice_disconnectChannel(channel);
    return status;
  }

  SpiceLinkAuthMechanism auth;
  auth.auth_mechanism = SPICE_COMMON_CAP_AUTH_SPICE;
  if (purespice_writeNL(channel, &auth, sizeof(auth)) != sizeof(auth))
  {
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  PSPassword pass;
  if (!purespice_rsaEncryptPassword(reply.pub_key, spice.password, &pass))
  {
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  if (purespice_writeNL(channel, pass.data, pass.size) != pass.size)
  {
    purespice_rsaFreePassword(&pass);
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  purespice_rsaFreePassword(&pass);

  uint32_t linkResult;
  if ((status = purespice_readNL(channel, &linkResult, sizeof(linkResult),
          NULL)) != PS_STATUS_OK)
  {
    purespice_disconnectChannel(channel);
    return status;
  }

  if (linkResult != SPICE_LINK_ERR_OK)
  {
    purespice_disconnectChannel(channel);
    return PS_STATUS_ERROR;
  }

  struct epoll_event ev =
  {
    .events   = EPOLLIN,
    .data.ptr = channel
  };
  epoll_ctl(spice.epollfd, EPOLL_CTL_ADD, channel->socket, &ev);

  channel->ready = true;
  return PS_STATUS_OK;
}

static void purespice_disconnectChannel(struct PSChannel * channel)
{
  if (!channel->connected)
    return;

  if (channel->ready)
  {
    /* disable nodelay so we can trigger a flush after this message */
    int flag;
    if (spice.family != AF_UNIX)
    {
      flag = 0;
      setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY,
          (char *)&flag, sizeof(int));
    }

    SpiceMsgcDisconnecting * packet = SPICE_PACKET(SPICE_MSGC_DISCONNECTING,
        SpiceMsgcDisconnecting, 0);
    packet->time_stamp = get_timestamp();
    packet->reason     = SPICE_LINK_ERR_OK;
    SPICE_SEND_PACKET(channel, packet);

    /* re-enable nodelay as this triggers a flush according to the man page */
    if (spice.family != AF_UNIX)
    {
      flag = 1;
      setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY,
          (char *)&flag, sizeof(int));
    }
  }

  epoll_ctl(spice.epollfd, EPOLL_CTL_DEL, channel->socket, NULL);
  shutdown(channel->socket, SHUT_WR);
}

static PS_STATUS purespice_agentConnect()
{
  if (!spice.agentQueue)
    spice.agentQueue = queue_new();
  else
  {
    void * msg;
    while(queue_shift(spice.agentQueue, &msg))
      SPICE_RAW_PACKET_FREE(msg);
  }

  uint32_t * packet = SPICE_PACKET(SPICE_MSGC_MAIN_AGENT_START, uint32_t, 0);
  memcpy(packet, &(uint32_t){SPICE_AGENT_TOKENS_MAX}, sizeof(uint32_t));
  if (!SPICE_SEND_PACKET(&spice.scMain, packet))
    return PS_STATUS_ERROR;

  spice.hasAgent = true;
  PS_STATUS ret = purespice_agentSendCaps(true);
  if (ret != PS_STATUS_OK)
  {
    spice.hasAgent = false;
    return ret;
  }

  return PS_STATUS_OK;
}

static PS_STATUS purespice_agentProcess(uint32_t dataSize, int * dataAvailable)
{
  PS_STATUS status;
  if (spice.cbRemain)
  {
    const uint32_t r = spice.cbRemain > dataSize ? dataSize : spice.cbRemain;
    if ((status = purespice_readNL(&spice.scMain, spice.cbBuffer + spice.cbSize, r,
            dataAvailable)) != PS_STATUS_OK)
    {
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbRemain = 0;
      spice.cbSize   = 0;
      return status;
    }

    spice.cbRemain -= r;
    spice.cbSize   += r;

    if (spice.cbRemain == 0)
      purespice_agentOnClipboard();

    return PS_STATUS_OK;
  }

  VDAgentMessage msg;

  #pragma pack(push,1)
  struct Selection
  {
    uint8_t selection;
    uint8_t reserved[3];
  };
  #pragma pack(pop)

  if ((status = purespice_readNL(&spice.scMain, &msg, sizeof(msg),
          dataAvailable)) != PS_STATUS_OK)
    return status;

  dataSize -= sizeof(msg);

  if (msg.protocol != VD_AGENT_PROTOCOL)
    return PS_STATUS_ERROR;

  switch(msg.type)
  {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
    {
      // make sure the message size is not insane to avoid a stack overflow
      // since we are using alloca for performance
      if (msg.size > 1024)
        return PS_STATUS_ERROR;

      VDAgentAnnounceCapabilities *caps =
        (VDAgentAnnounceCapabilities *)alloca(msg.size);

      if ((status = purespice_readNL(&spice.scMain, caps, msg.size,
              dataAvailable)) != PS_STATUS_OK)
        return status;

      const int capsSize = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(msg.size);
      spice.cbSupported  =
        VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize,
            VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) ||
        VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize,
            VD_AGENT_CAP_CLIPBOARD_SELECTION);

      spice.cbSelection  =
        VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize,
            VD_AGENT_CAP_CLIPBOARD_SELECTION);

      if (caps->request)
        return purespice_agentSendCaps(false);

      return PS_STATUS_OK;
    }

    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_RELEASE:
    {
      uint32_t remaining = msg.size;
      if (spice.cbSelection)
      {
        struct Selection selection;
        if ((status = purespice_readNL(&spice.scMain, &selection, sizeof(selection),
                dataAvailable)) != PS_STATUS_OK)
          return status;
        remaining -= sizeof(selection);
        dataSize  -= sizeof(selection);
      }

      if (msg.type == VD_AGENT_CLIPBOARD_RELEASE)
      {
        spice.cbAgentGrabbed = false;
        if (spice.cbReleaseFn)
          spice.cbReleaseFn();
        return PS_STATUS_OK;
      }

      if (msg.type == VD_AGENT_CLIPBOARD ||
          msg.type == VD_AGENT_CLIPBOARD_REQUEST)
      {
        uint32_t type;
        if ((status = purespice_readNL(&spice.scMain, &type, sizeof(type),
                dataAvailable)) != PS_STATUS_OK)
          return status;
        remaining -= sizeof(type);
        dataSize  -= sizeof(type);

        if (msg.type == VD_AGENT_CLIPBOARD)
        {
          if (spice.cbBuffer)
            return PS_STATUS_ERROR;

          spice.cbSize     = 0;
          spice.cbRemain   = remaining;
          spice.cbBuffer   = (uint8_t *)malloc(remaining);
          const uint32_t r = remaining > dataSize ? dataSize : remaining;

          if ((status = purespice_readNL(&spice.scMain, spice.cbBuffer, r,
                  dataAvailable)) != PS_STATUS_OK)
          {
            free(spice.cbBuffer);
            spice.cbBuffer = NULL;
            spice.cbRemain = 0;
            spice.cbSize   = 0;
            return status;
          }

          spice.cbRemain -= r;
          spice.cbSize   += r;

          if (spice.cbRemain == 0)
            purespice_agentOnClipboard();

          return PS_STATUS_OK;
        }
        else
        {
          if (spice.cbRequestFn)
            spice.cbRequestFn(agent_type_to_purespice_type(type));
          return PS_STATUS_OK;
        }
      }
      else
      {
        if (remaining == 0)
          return PS_STATUS_OK;

        // ensure the size is sane to avoid a stack overflow since we use alloca
        // for performance
        if (remaining > 1024)
          return PS_STATUS_ERROR;

        uint32_t *types = alloca(remaining);
        if ((status = purespice_readNL(&spice.scMain, types, remaining,
                dataAvailable)) != PS_STATUS_OK)
          return status;

        // there is zero documentation on the types field, it might be a bitfield
        // but for now we are going to assume it's not.

        spice.cbType          = agent_type_to_purespice_type(types[0]);
        spice.cbAgentGrabbed  = true;
        spice.cbClientGrabbed = false;
        if (spice.cbSelection)
        {
          // Windows doesnt support this, so until it's needed there is no point
          // messing with it
          return PS_STATUS_OK;
        }

        if (spice.cbNoticeFn)
            spice.cbNoticeFn(spice.cbType);

        return PS_STATUS_OK;
      }
    }
  }

  return purespice_discardNL(&spice.scMain, msg.size, dataAvailable);
}


static void purespice_agentOnClipboard()
{
  if (spice.cbDataFn)
    spice.cbDataFn(spice.cbType, spice.cbBuffer, spice.cbSize);

  free(spice.cbBuffer);
  spice.cbBuffer = NULL;
  spice.cbSize   = 0;
  spice.cbRemain = 0;
}

static PS_STATUS purespice_agentSendCaps(bool request)
{
  if (!spice.hasAgent)
    return PS_STATUS_ERROR;

  const ssize_t capsSize = sizeof(VDAgentAnnounceCapabilities) +
    VD_AGENT_CAPS_BYTES;
  VDAgentAnnounceCapabilities *caps =
    (VDAgentAnnounceCapabilities *)alloca(capsSize);
  memset(caps, 0, capsSize);

  caps->request = request ? 1 : 0;
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);

  if (!purespice_agentStartMsg(VD_AGENT_ANNOUNCE_CAPABILITIES, capsSize) ||
      !purespice_agentWriteMsg(caps, capsSize))
    return PS_STATUS_ERROR;

  return PS_STATUS_OK;
}

static bool purespice_takeServerToken(void)
{
  uint32_t tokens;
  do
  {
    if (!spice.scMain.connected)
      return false;

    tokens = atomic_load(&spice.serverTokens);
    if (tokens == 0)
      return false;
  }
  while(!atomic_compare_exchange_weak(&spice.serverTokens, &tokens, tokens - 1));

  return true;
}

static bool purespice_agentProcessQueue(void)
{
  SPICE_LOCK(spice.scMain.lock);
  while (queue_peek(spice.agentQueue, NULL) && purespice_takeServerToken())
  {
    void * msg;
    queue_shift(spice.agentQueue, &msg);
    if (!SPICE_SEND_PACKET_NL(&spice.scMain, msg))
    {
      SPICE_RAW_PACKET_FREE(msg);
      SPICE_UNLOCK(spice.scMain.lock);
      return false;
    }
    SPICE_RAW_PACKET_FREE(msg);
  }
  SPICE_UNLOCK(spice.scMain.lock);
  return true;
}

static bool purespice_agentStartMsg(uint32_t type, ssize_t size)
{
  VDAgentMessage * msg =
    SPICE_PACKET_MALLOC(SPICE_MSGC_MAIN_AGENT_DATA, VDAgentMessage, 0);

  msg->protocol  = VD_AGENT_PROTOCOL;
  msg->type      = type;
  msg->opaque    = 0;
  msg->size      = size;
  spice.agentMsg = size;
  queue_push(spice.agentQueue, msg);

  return purespice_agentProcessQueue();
}

static bool purespice_agentWriteMsg(const void * buffer_, ssize_t size)
{
  assert(size <= spice.agentMsg);

  const char * buffer = buffer_;
  while(size)
  {
    const ssize_t toWrite = size > VD_AGENT_MAX_DATA_SIZE ?
      VD_AGENT_MAX_DATA_SIZE : size;

    void * msg = SPICE_RAW_PACKET_MALLOC(SPICE_MSGC_MAIN_AGENT_DATA, toWrite, 0);
    memcpy(msg, buffer, toWrite);
    queue_push(spice.agentQueue, msg);

    size           -= toWrite;
    buffer         += toWrite;
    spice.agentMsg -= toWrite;
  }

  return purespice_agentProcessQueue();
}

static ssize_t purespice_writeNL(const struct PSChannel * channel,
    const void * buffer, const ssize_t size)
{
  if (!channel->connected)
    return -1;

  if (!buffer)
    return -1;

  return send(channel->socket, buffer, size, 0);
}

static PS_STATUS purespice_readNL(struct PSChannel * channel, void * buffer,
    const ssize_t size, int * dataAvailable)
{
  if (!channel->connected)
    return PS_STATUS_ERROR;

  if (!buffer)
    return PS_STATUS_ERROR;

  size_t    left = size;
  uint8_t * buf  = (uint8_t *)buffer;
  while(left)
  {
    ssize_t len = read(channel->socket, buf, left);
    if (len == 0)
      return PS_STATUS_NODATA;

    if (len < 0)
    {
      channel->connected = false;
      return PS_STATUS_ERROR;
    }
    left -= len;
    buf  += len;

    if (dataAvailable)
      *dataAvailable -= len;
  }

  return PS_STATUS_OK;
}

static PS_STATUS purespice_discardNL(struct PSChannel * channel,
    ssize_t size, int * dataAvailable)
{
  uint8_t c[1024];
  ssize_t left = size;
  while(left)
  {
    ssize_t len = read(channel->socket, c, left > sizeof(c) ? sizeof(c) : left);
    if (len == 0)
      return PS_STATUS_NODATA;

    if (len < 0)
    {
      channel->connected = false;
      return PS_STATUS_ERROR;
    }

    left -= len;

    if (dataAvailable)
      *dataAvailable -= len;
  }

  return PS_STATUS_OK;
}

bool purespice_keyDown(uint32_t code)
{
  if (!spice.scInputs.connected)
    return false;

  if (code > 0x100)
    code = 0xe0 | ((code - 0x100) << 8);

  SpiceMsgcKeyDown * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_DOWN,
        SpiceMsgcKeyDown, 0);
  msg->code = code;
  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

bool purespice_keyUp(uint32_t code)
{
  if (!spice.scInputs.connected)
    return false;

  if (code < 0x100)
    code |= 0x80;
  else
    code = 0x80e0 | ((code - 0x100) << 8);

  SpiceMsgcKeyUp * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_UP,
        SpiceMsgcKeyUp, 0);
  msg->code = code;
  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

bool purespice_keyModifiers(uint32_t modifiers)
{
  if (!spice.scInputs.connected)
    return false;

  SpiceMsgcInputsKeyModifiers * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_MODIFIERS,
        SpiceMsgcInputsKeyModifiers, 0);
  msg->modifiers = modifiers;
  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

bool purespice_mouseMode(bool server)
{
  if (!spice.scMain.connected)
    return false;

  SpiceMsgcMainMouseModeRequest * msg = SPICE_PACKET(
    SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST,
    SpiceMsgcMainMouseModeRequest, 0);

  msg->mouse_mode = server ? SPICE_MOUSE_MODE_SERVER : SPICE_MOUSE_MODE_CLIENT;
  return SPICE_SEND_PACKET(&spice.scMain, msg);
}

bool purespice_mousePosition(uint32_t x, uint32_t y)
{
  if (!spice.scInputs.connected)
    return false;

  SpiceMsgcMousePosition * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_POSITION, SpiceMsgcMousePosition, 0);

  SPICE_LOCK(spice.mouse.lock);
  msg->display_id   = 0;
  msg->button_state = spice.mouse.buttonState;
  msg->x            = x;
  msg->y            = y;
  SPICE_UNLOCK(spice.mouse.lock);

  atomic_fetch_add(&spice.mouse.sentCount, 1);
  if (!SPICE_SEND_PACKET(&spice.scInputs, msg))
    return false;

  return true;
}

bool purespice_mouseMotion(int32_t x, int32_t y)
{
  if (!spice.scInputs.connected)
    return false;

  /* while the protocol supports movements greater then +-127 the QEMU
   * virtio-mouse device does not, so we need to split this up into seperate
   * messages. For performance we build this as a single buffer otherwise this
   * will be split into multiple packets */

  const unsigned delta = abs(x) > abs(y) ? abs(x) : abs(y);
  const unsigned msgs  = (delta + 126) / 127;

  // only one message, so just send it normally
  if (msgs == 1)
  {
    SpiceMsgcMouseMotion * msg =
      SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_MOTION, SpiceMsgcMouseMotion, 0);

    SPICE_LOCK(spice.mouse.lock);
    msg->x            = x;
    msg->y            = y;
    msg->button_state = spice.mouse.buttonState;
    SPICE_UNLOCK(spice.mouse.lock);

    atomic_fetch_add(&spice.mouse.sentCount, 1);
    return SPICE_SEND_PACKET(&spice.scInputs, msg);
  }

  const ssize_t bufferSize = (
    sizeof(SpiceMiniDataHeader ) +
    sizeof(SpiceMsgcMouseMotion)
  ) * msgs;

  if (bufferSize > spice.motionBufferSize)
  {
    if (spice.motionBuffer)
      free(spice.motionBuffer);
    spice.motionBuffer     = malloc(bufferSize);
    spice.motionBufferSize = bufferSize;
  }

  uint8_t * buffer = spice.motionBuffer;
  uint8_t * msg    = buffer;

  SPICE_LOCK(spice.mouse.lock);
  while(x != 0 || y != 0)
  {
    SpiceMiniDataHeader  *h = (SpiceMiniDataHeader  *)msg;
    SpiceMsgcMouseMotion *m = (SpiceMsgcMouseMotion *)(h + 1);
    msg = (uint8_t*)(m + 1);

    h->size = sizeof(SpiceMsgcMouseMotion);
    h->type = SPICE_MSGC_INPUTS_MOUSE_MOTION;

    m->x = x > 127 ? 127 : (x < -127 ? -127 : x);
    m->y = y > 127 ? 127 : (y < -127 ? -127 : y);
    m->button_state = spice.mouse.buttonState;

    x -= m->x;
    y -= m->y;
  }
  SPICE_UNLOCK(spice.mouse.lock);

  atomic_fetch_add(&spice.mouse.sentCount, msgs);

  SPICE_LOCK(spice.scInputs.lock);
  const ssize_t wrote = send(spice.scInputs.socket, buffer, bufferSize, 0);
  SPICE_UNLOCK(spice.scInputs.lock);

  return wrote == bufferSize;
}

bool purespice_mousePress(uint32_t button)
{
  if (!spice.scInputs.connected)
    return false;

  SPICE_LOCK(spice.mouse.lock);
  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT   :
      spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_LEFT   ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE :
      spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_MIDDLE ; break;
    case SPICE_MOUSE_BUTTON_RIGHT  :
      spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_RIGHT  ; break;
    case _SPICE_MOUSE_BUTTON_SIDE  :
      spice.mouse.buttonState |= _SPICE_MOUSE_BUTTON_MASK_SIDE  ; break;
    case _SPICE_MOUSE_BUTTON_EXTRA :
      spice.mouse.buttonState |= _SPICE_MOUSE_BUTTON_MASK_EXTRA ; break;
  }

  SpiceMsgcMousePress * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_PRESS, SpiceMsgcMousePress, 0);

  msg->button       = button;
  msg->button_state = spice.mouse.buttonState;
  SPICE_UNLOCK(spice.mouse.lock);

  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

bool purespice_mouseRelease(uint32_t button)
{
  if (!spice.scInputs.connected)
    return false;

  SPICE_LOCK(spice.mouse.lock);
  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT   :
      spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_LEFT   ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE :
      spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_MIDDLE ; break;
    case SPICE_MOUSE_BUTTON_RIGHT  :
      spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_RIGHT  ; break;
    case _SPICE_MOUSE_BUTTON_SIDE  :
      spice.mouse.buttonState &= ~_SPICE_MOUSE_BUTTON_MASK_SIDE  ; break;
    case _SPICE_MOUSE_BUTTON_EXTRA :
      spice.mouse.buttonState &= ~_SPICE_MOUSE_BUTTON_MASK_EXTRA ; break;
  }

  SpiceMsgcMouseRelease * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_RELEASE, SpiceMsgcMouseRelease, 0);

  msg->button       = button;
  msg->button_state = spice.mouse.buttonState;
  SPICE_UNLOCK(spice.mouse.lock);

  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

static uint32_t purespice_typeToAgentType(PSDataType type)
{
  switch(type)
  {
    case SPICE_DATA_TEXT: return VD_AGENT_CLIPBOARD_UTF8_TEXT ; break;
    case SPICE_DATA_PNG : return VD_AGENT_CLIPBOARD_IMAGE_PNG ; break;
    case SPICE_DATA_BMP : return VD_AGENT_CLIPBOARD_IMAGE_BMP ; break;
    case SPICE_DATA_TIFF: return VD_AGENT_CLIPBOARD_IMAGE_TIFF; break;
    case SPICE_DATA_JPEG: return VD_AGENT_CLIPBOARD_IMAGE_JPG ; break;
    default:
      return VD_AGENT_CLIPBOARD_NONE;
  }
}

static PSDataType agent_type_to_purespice_type(uint32_t type)
{
  switch(type)
  {
    case VD_AGENT_CLIPBOARD_UTF8_TEXT : return SPICE_DATA_TEXT; break;
    case VD_AGENT_CLIPBOARD_IMAGE_PNG : return SPICE_DATA_PNG ; break;
    case VD_AGENT_CLIPBOARD_IMAGE_BMP : return SPICE_DATA_BMP ; break;
    case VD_AGENT_CLIPBOARD_IMAGE_TIFF: return SPICE_DATA_TIFF; break;
    case VD_AGENT_CLIPBOARD_IMAGE_JPG : return SPICE_DATA_JPEG; break;
    default:
      return SPICE_DATA_NONE;
  }
}

bool purespice_clipboardRequest(PSDataType type)
{
  if (!spice.hasAgent)
    return false;

  VDAgentClipboardRequest req;

  if (!spice.cbAgentGrabbed)
    return false;

  if (type != spice.cbType)
    return false;

  req.type = purespice_typeToAgentType(type);
  if (!purespice_agentStartMsg(VD_AGENT_CLIPBOARD_REQUEST, sizeof(req)) ||
      !purespice_agentWriteMsg(&req, sizeof(req)))
    return false;

  return true;
}

bool purespice_setClipboardCb(
    PSClipboardNotice  cbNoticeFn,
    PSClipboardData    cbDataFn,
    PSClipboardRelease cbReleaseFn,
    PSClipboardRequest cbRequestFn)
{
  if ((cbNoticeFn && !cbDataFn) || (cbDataFn && !cbNoticeFn))
    return false;

  spice.cbNoticeFn  = cbNoticeFn;
  spice.cbDataFn    = cbDataFn;
  spice.cbReleaseFn = cbReleaseFn;
  spice.cbRequestFn = cbRequestFn;

  return true;
}

bool purespice_clipboardGrab(PSDataType types[], int count)
{
  if (!spice.hasAgent)
    return false;

  if (count == 0)
    return false;

  if (spice.cbSelection)
  {
    struct Msg
    {
      uint8_t  selection;
      uint8_t  reserved;
      uint32_t types[0];
    };

    const int size = sizeof(struct Msg) + count * sizeof(uint32_t);
    struct Msg * msg = alloca(size);
    msg->selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    msg->reserved  = 0;
    for(int i = 0; i < count; ++i)
      msg->types[i] = purespice_typeToAgentType(types[i]);

    if (!purespice_agentStartMsg(VD_AGENT_CLIPBOARD_GRAB, size) ||
        !purespice_agentWriteMsg(msg, size))
      return false;

    spice.cbClientGrabbed = true;
    return true;
  }

  uint32_t msg[count];
  for(int i = 0; i < count; ++i)
    msg[i] = purespice_typeToAgentType(types[i]);

  if (!purespice_agentStartMsg(VD_AGENT_CLIPBOARD_GRAB, sizeof(msg)) ||
      !purespice_agentWriteMsg(&msg, sizeof(msg)))
    return false;

  spice.cbClientGrabbed = true;
  return true;
}

bool purespice_clipboardRelease()
{
  if (!spice.hasAgent)
    return false;

  // check if if there is anything to release first
  if (!spice.cbClientGrabbed)
    return true;

  if (spice.cbSelection)
  {
    uint8_t req[4] = { VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD };
    if (!purespice_agentStartMsg(VD_AGENT_CLIPBOARD_RELEASE, sizeof(req)) ||
        !purespice_agentWriteMsg(req, sizeof(req)))
      return false;

    spice.cbClientGrabbed = false;
    return true;
  }

   if (!purespice_agentStartMsg(VD_AGENT_CLIPBOARD_RELEASE, 0))
     return false;

   spice.cbClientGrabbed = false;
   return true;
}

bool purespice_clipboardDataStart(PSDataType type, size_t size)
{
  if (!spice.hasAgent)
    return false;

  uint8_t buffer[8];
  size_t  bufSize;

  if (spice.cbSelection)
  {
    bufSize                = 8;
    buffer[0]              = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    buffer[1]              = buffer[2] = buffer[3] = 0;
    ((uint32_t*)buffer)[1] = purespice_typeToAgentType(type);
  }
  else
  {
    bufSize                = 4;
    ((uint32_t*)buffer)[0] = purespice_typeToAgentType(type);
  }

  return purespice_agentStartMsg(VD_AGENT_CLIPBOARD, bufSize + size) &&
    purespice_agentWriteMsg(buffer, bufSize);
}

bool purespice_clipboardData(PSDataType type, uint8_t * data, size_t size)
{
  if (!spice.hasAgent)
    return false;

  return purespice_agentWriteMsg(data, size);
}

bool purespice_setAudioCb(
  void (*start)(int channels, int sampleRate, PSAudioFormat format,
    uint32_t time),
  void (*volume)(int channels, const uint16_t volume[]),
  void (*mute)(bool mute),
  void (*stop)(void),
  void (*data)(uint8_t * data, size_t size)
)
{
  if (!start || !stop || !data)
    return false;

  spice.playbackStart  = start;
  spice.playbackVolume = volume;
  spice.playbackMute   = mute;
  spice.playbackStop   = stop;
  spice.playbackData   = data;

  return true;
}
