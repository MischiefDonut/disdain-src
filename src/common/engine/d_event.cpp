/*
** c_dispatch.cpp
** Functions for executing console commands and aliases
**
**---------------------------------------------------------------------------
** Copyright 1998-2016 Randy Heit
** Copyright 2003-2019 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/ 

#include "c_bind.h"
#include "d_eventbase.h"
#include "c_console.h"
#include "d_gui.h"
#include "menu.h"
#include "utf8.h"
#include "m_joy.h"
#include "vm.h"
#include "gamestate.h"
#include "i_interface.h"

int eventhead;
int eventtail;
event_t events[MAXEVENTS];

CVAR(Float, m_sensitivity_x, 2.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, m_sensitivity_y, 2.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, invertmouse, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE);  // Invert mouse look down/up?
CVAR(Bool, invertmousex, false,	CVAR_GLOBALCONFIG | CVAR_ARCHIVE);  // Invert mouse look left/right?


//==========================================================================
//
// D_ProcessEvents
//
// Send all the events of the given timestamp down the responder chain.
// Events are asynchronous inputs generally generated by the game user.
// Events can be discarded if no responder claims them
//
//==========================================================================

void D_ProcessEvents (void)
{
	FixedBitArray<NUM_KEYS> keywasdown;
	TArray<event_t*> delayedevents;

	keywasdown.Zero();
	while (eventtail != eventhead)
	{
		event_t *ev = &events[eventtail];
		eventtail = (eventtail + 1) & (MAXEVENTS - 1);

		if (ev->type == EV_KeyUp && keywasdown[ev->data1])
		{
			delayedevents.Push(ev);
			continue;
		}

		if (ev->type == EV_None)
			continue;
		if (ev->type == EV_DeviceChange)
			UpdateJoystickMenu(I_UpdateDeviceList());

		// allow the game to intercept Escape before dispatching it.
		if (ev->type != EV_KeyDown || ev->data1 != KEY_ESCAPE || !sysCallbacks.WantEscape || !sysCallbacks.WantEscape())
		{
			if (gamestate != GS_INTRO) // GS_INTRO blocks the UI.
			{
				if (C_Responder(ev))
					continue;				// console ate the event
				if (M_Responder(ev))
					continue;				// menu ate the event
			}
		}

		if (sysCallbacks.G_Responder(ev) && ev->type == EV_KeyDown) keywasdown.Set(ev->data1);
	}

	for (auto ev: delayedevents)
	{
		D_PostEvent(ev);
	}
}

//==========================================================================
//
// D_RemoveNextCharEvent
//
// Removes the next EV_GUI_Char event in the input queue. Used by the menu,
// since it (generally) consumes EV_GUI_KeyDown events and not EV_GUI_Char
// events, and it needs to ensure that there is no left over input when it's
// done. If there are multiple EV_GUI_KeyDowns before the EV_GUI_Char, then
// there are dead chars involved, so those should be removed, too. We do
// this by changing the message type to EV_None rather than by actually
// removing the event from the queue.
// 
//==========================================================================

void D_RemoveNextCharEvent()
{
	assert(events[eventtail].type == EV_GUI_Event && events[eventtail].subtype == EV_GUI_KeyDown);
	for (int evnum = eventtail; evnum != eventhead; evnum = (evnum+1) & (MAXEVENTS-1))
	{
		event_t *ev = &events[evnum];
		if (ev->type != EV_GUI_Event)
			break;
		if (ev->subtype == EV_GUI_KeyDown || ev->subtype == EV_GUI_Char)
		{
			ev->type = EV_None;
			if (ev->subtype == EV_GUI_Char)
				break;
		}
		else
		{
			break;
		}
	}
}


//==========================================================================
//
// D_PostEvent
//
// Called by the I/O functions when input is detected.
//
//==========================================================================

void D_PostEvent(event_t* ev)
{
	// Do not post duplicate consecutive EV_DeviceChange events.
	if (ev->type == EV_DeviceChange && events[eventhead].type == EV_DeviceChange)
	{
		return;
	}
	if (sysCallbacks.DispatchEvent && sysCallbacks.DispatchEvent(ev))
		return;

	events[eventhead] = *ev;
	eventhead = (eventhead + 1) & (MAXEVENTS - 1);
}


void PostMouseMove(int xx, int yy)
{
	event_t ev{};

	ev.x = float(xx) * m_sensitivity_x;
	ev.y = -float(yy) * m_sensitivity_y;

	if (invertmousex) ev.x = -ev.x;
	if (invertmouse) ev.y = -ev.y;

	if (ev.x || ev.y)
	{
		ev.type = EV_Mouse;
		D_PostEvent(&ev);
	}
}


FInputEvent::FInputEvent(const event_t *ev)
{
	Type = (EGenericEvent)ev->type;
	// we don't want the modders to remember what weird fields mean what for what events.
	KeyScan = 0;
	KeyChar = 0;
	MouseX = 0;
	MouseY = 0;
	switch (Type)
	{
	case EV_None:
		break;
	case EV_KeyDown:
	case EV_KeyUp:
		KeyScan = ev->data1;
		KeyChar = ev->data2;
		KeyString = FString(char(ev->data1));
		break;
	case EV_Mouse:
		MouseX = int(ev->x);
		MouseY = int(ev->y);
		break;
	default:
		break; // EV_DeviceChange = wat?
	}
}

FUiEvent::FUiEvent(const event_t *ev)
{
	Type = (EGUIEvent)ev->subtype;
	KeyChar = 0;
	IsShift = false;
	IsAlt = false;
	IsCtrl = false;
	MouseX = 0;
	MouseY = 0;
	// we don't want the modders to remember what weird fields mean what for what events.
	switch (ev->subtype)
	{
	case EV_GUI_None:
		break;
	case EV_GUI_KeyDown:
	case EV_GUI_KeyRepeat:
	case EV_GUI_KeyUp:
		KeyChar = ev->data1;
		KeyString = FString(char(ev->data1));
		IsShift = !!(ev->data3 & GKM_SHIFT);
		IsAlt = !!(ev->data3 & GKM_ALT);
		IsCtrl = !!(ev->data3 & GKM_CTRL);
		break;
	case EV_GUI_Char:
		KeyChar = ev->data1;
		KeyString = MakeUTF8(ev->data1);
		IsAlt = !!ev->data2; // only true for Win32, not sure about SDL
		break;
	default: // mouse event
			 // note: SDL input doesn't seem to provide these at all
			 //Printf("Mouse data: %d, %d, %d, %d\n", ev->x, ev->y, ev->data1, ev->data2);
		MouseX = ev->data1;
		MouseY = ev->data2;
		IsShift = !!(ev->data3 & GKM_SHIFT);
		IsAlt = !!(ev->data3 & GKM_ALT);
		IsCtrl = !!(ev->data3 & GKM_CTRL);
		break;
	}
}

DEFINE_FIELD_X(UiEvent, FUiEvent, Type);
DEFINE_FIELD_X(UiEvent, FUiEvent, KeyString);
DEFINE_FIELD_X(UiEvent, FUiEvent, KeyChar);
DEFINE_FIELD_X(UiEvent, FUiEvent, MouseX);
DEFINE_FIELD_X(UiEvent, FUiEvent, MouseY);
DEFINE_FIELD_X(UiEvent, FUiEvent, IsShift);
DEFINE_FIELD_X(UiEvent, FUiEvent, IsAlt);
DEFINE_FIELD_X(UiEvent, FUiEvent, IsCtrl);

DEFINE_FIELD_X(InputEvent, FInputEvent, Type);
DEFINE_FIELD_X(InputEvent, FInputEvent, KeyScan);
DEFINE_FIELD_X(InputEvent, FInputEvent, KeyString);
DEFINE_FIELD_X(InputEvent, FInputEvent, KeyChar);
DEFINE_FIELD_X(InputEvent, FInputEvent, MouseX);
DEFINE_FIELD_X(InputEvent, FInputEvent, MouseY);

