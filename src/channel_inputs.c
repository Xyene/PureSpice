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

#include "purespice.h"

#include "ps.h"
#include "log.h"
#include "channel.h"
#include "messages.h"

#include <stdlib.h>

const SpiceLinkHeader * channelInputs_getConnectPacket(void)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[INPUT_CAPS_BYTES   / sizeof(uint32_t)];
  }
  __attribute__((packed)) ConnectPacket;

  static ConnectPacket p =
  {
    .header = {
      .magic         = SPICE_MAGIC        ,
      .major_version = SPICE_VERSION_MAJOR,
      .minor_version = SPICE_VERSION_MINOR,
      .size          = sizeof(ConnectPacket) - sizeof(SpiceLinkHeader)
    },
    .message = {
      .channel_type     = SPICE_CHANNEL_INPUTS,
      .num_common_caps  = COMMON_CAPS_BYTES / sizeof(uint32_t),
      .num_channel_caps = INPUT_CAPS_BYTES  / sizeof(uint32_t),
      .caps_offset      = sizeof(SpiceLinkMess)
    }
  };

  p.message.connection_id = g_ps.sessionID;
  p.message.channel_id    = g_ps.channelID;

  memset(p.supportCaps, 0, sizeof(p.supportCaps));
  memset(p.channelCaps, 0, sizeof(p.channelCaps));

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  return &p.header;
}

PS_STATUS channelInputs_onRead(struct PSChannel * channel, int * dataAvailable)
{
  SpiceMiniDataHeader header;

  PS_STATUS status;
  if ((status = channel_onRead(channel, &header,
          dataAvailable)) != PS_STATUS_OK)
    return status;

  switch(header.type)
  {
    case SPICE_MSG_INPUTS_INIT:
    {
      if (channel->initDone)
      {
        PS_LOG_ERROR("Got SPICE_MSG_INPUTS_INIT before initDone");
        return PS_STATUS_ERROR;
      }

      channel->initDone = true;

      SpiceMsgInputsInit in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgInputsInit");
        return status;
      }

      return PS_STATUS_OK;
    }

    case SPICE_MSG_INPUTS_KEY_MODIFIERS:
    {
      SpiceMsgInputsInit in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgInputsInit");
        return status;
      }

      g_ps.kb.modifiers = in.modifiers;
      return PS_STATUS_OK;
    }

    case SPICE_MSG_INPUTS_MOUSE_MOTION_ACK:
    {
      const int count = atomic_fetch_sub(&g_ps.mouse.sentCount,
          SPICE_INPUT_MOTION_ACK_BUNCH);
      if (count < SPICE_INPUT_MOTION_ACK_BUNCH)
      {
        PS_LOG_ERROR("Server sent an ack for more messages then expected");
        return PS_STATUS_ERROR;
      }

      return PS_STATUS_OK;
    }
  }

  if ((status = channel_discardNL(channel, header.size,
          dataAvailable)) != PS_STATUS_OK)
  {
    PS_LOG_ERROR("Failed to discard %d bytes", header.size);
    return status;
  }

  return PS_STATUS_OK;
}

bool purespice_keyDown(uint32_t code)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_INPUTS];
  if (!channel->connected || !channel->ready)
    return false;

  if (code > 0x100)
    code = 0xe0 | ((code - 0x100) << 8);

  SpiceMsgcKeyDown * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_DOWN,
        SpiceMsgcKeyDown, 0);
  msg->code = code;

  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to send SpiceMsgcKeyDown");
    return false;
  }

  return true;
}

bool purespice_keyUp(uint32_t code)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_INPUTS];
  if (!channel->connected || !channel->ready)
    return false;

  if (code < 0x100)
    code |= 0x80;
  else
    code = 0x80e0 | ((code - 0x100) << 8);

  SpiceMsgcKeyUp * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_UP,
        SpiceMsgcKeyUp, 0);
  msg->code = code;

  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to send SpiceMsgcKeyUp");
    return false;
  }

  return true;
}

bool purespice_keyModifiers(uint32_t modifiers)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_INPUTS];
  if (!channel->connected || !channel->ready)
    return false;

  SpiceMsgcInputsKeyModifiers * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_MODIFIERS,
        SpiceMsgcInputsKeyModifiers, 0);
  msg->modifiers = modifiers;

  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to send SpiceMsgcInputsKeyModifiers");
    return false;
  }

  return true;
}

bool purespice_mouseMode(bool server)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_MAIN];
  if (!channel->connected || !channel->ready)
    return false;

  SpiceMsgcMainMouseModeRequest * msg = SPICE_PACKET(
    SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST,
    SpiceMsgcMainMouseModeRequest, 0);

  msg->mouse_mode = server ? SPICE_MOUSE_MODE_SERVER : SPICE_MOUSE_MODE_CLIENT;

  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to send SpiceMsgcMainMouseModeRequest");
    return false;
  }

  return true;
}

bool purespice_mousePosition(uint32_t x, uint32_t y)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_INPUTS];
  if (!channel->connected || !channel->ready)
    return false;

  SpiceMsgcMousePosition * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_POSITION, SpiceMsgcMousePosition, 0);

  SPICE_LOCK(g_ps.mouse.lock);
  msg->display_id   = 0;
  msg->button_state = g_ps.mouse.buttonState;
  msg->x            = x;
  msg->y            = y;
  SPICE_UNLOCK(g_ps.mouse.lock);

  atomic_fetch_add(&g_ps.mouse.sentCount, 1);
  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to send SpiceMsgcMousePosition");
    return false;
  }

  return true;
}

bool purespice_mouseMotion(int32_t x, int32_t y)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_INPUTS];
  if (!channel->connected || !channel->ready)
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

    SPICE_LOCK(g_ps.mouse.lock);
    msg->x            = x;
    msg->y            = y;
    msg->button_state = g_ps.mouse.buttonState;
    SPICE_UNLOCK(g_ps.mouse.lock);

    atomic_fetch_add(&g_ps.mouse.sentCount, 1);
    if (!SPICE_SEND_PACKET(channel, msg))
    {
      PS_LOG_ERROR("Failed to send SpiceMsgcMouseMotion");
      return false;
    }

    return true;
  }

  const size_t bufferSize = (
    sizeof(SpiceMiniDataHeader ) +
    sizeof(SpiceMsgcMouseMotion)
  ) * msgs;

  if (bufferSize > g_ps.motionBufferSize)
  {
    if (g_ps.motionBuffer)
      free(g_ps.motionBuffer);
    g_ps.motionBuffer     = malloc(bufferSize);
    g_ps.motionBufferSize = bufferSize;
  }

  uint8_t * buffer = g_ps.motionBuffer;
  uint8_t * msg    = buffer;

  SPICE_LOCK(g_ps.mouse.lock);
  while(x != 0 || y != 0)
  {
    SpiceMiniDataHeader  *h = (SpiceMiniDataHeader  *)msg;
    SpiceMsgcMouseMotion *m = (SpiceMsgcMouseMotion *)(h + 1);
    msg = (uint8_t*)(m + 1);

    h->size = sizeof(SpiceMsgcMouseMotion);
    h->type = SPICE_MSGC_INPUTS_MOUSE_MOTION;

    m->x = x > 127 ? 127 : (x < -127 ? -127 : x);
    m->y = y > 127 ? 127 : (y < -127 ? -127 : y);
    m->button_state = g_ps.mouse.buttonState;

    x -= m->x;
    y -= m->y;
  }
  SPICE_UNLOCK(g_ps.mouse.lock);

  atomic_fetch_add(&g_ps.mouse.sentCount, msgs);

  SPICE_LOCK(channel->lock);
  const ssize_t wrote = send(channel->socket, buffer, bufferSize, 0);
  SPICE_UNLOCK(channel->lock);

  if ((size_t)wrote != bufferSize)
  {
    PS_LOG_ERROR("Only wrote %ld of the expected %ld bytes", wrote, bufferSize);
    return false;
  }

  return true;
}

bool purespice_mousePress(uint32_t button)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_INPUTS];
  if (!channel->connected || !channel->ready)
    return false;

  SPICE_LOCK(g_ps.mouse.lock);
  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT   :
      g_ps.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_LEFT   ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE :
      g_ps.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_MIDDLE ; break;
    case SPICE_MOUSE_BUTTON_RIGHT  :
      g_ps.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_RIGHT  ; break;
    case _SPICE_MOUSE_BUTTON_SIDE  :
      g_ps.mouse.buttonState |= _SPICE_MOUSE_BUTTON_MASK_SIDE  ; break;
    case _SPICE_MOUSE_BUTTON_EXTRA :
      g_ps.mouse.buttonState |= _SPICE_MOUSE_BUTTON_MASK_EXTRA ; break;
  }

  SpiceMsgcMousePress * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_PRESS, SpiceMsgcMousePress, 0);

  msg->button       = button;
  msg->button_state = g_ps.mouse.buttonState;
  SPICE_UNLOCK(g_ps.mouse.lock);

  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to write SpiceMsgcMousePress");
    return false;
  }

  return true;
}

bool purespice_mouseRelease(uint32_t button)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_INPUTS];
  if (!channel->connected || !channel->ready)
    return false;

  SPICE_LOCK(g_ps.mouse.lock);
  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT   :
      g_ps.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_LEFT   ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE :
      g_ps.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_MIDDLE ; break;
    case SPICE_MOUSE_BUTTON_RIGHT  :
      g_ps.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_RIGHT  ; break;
    case _SPICE_MOUSE_BUTTON_SIDE  :
      g_ps.mouse.buttonState &= ~_SPICE_MOUSE_BUTTON_MASK_SIDE  ; break;
    case _SPICE_MOUSE_BUTTON_EXTRA :
      g_ps.mouse.buttonState &= ~_SPICE_MOUSE_BUTTON_MASK_EXTRA ; break;
  }

  SpiceMsgcMouseRelease * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_RELEASE, SpiceMsgcMouseRelease, 0);

  msg->button       = button;
  msg->button_state = g_ps.mouse.buttonState;
  SPICE_UNLOCK(g_ps.mouse.lock);

  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to write SpiceMsgcMouseRelease");
    return false;
  }

  return true;
}
