/*
Copyright (C) 2026 the PulseX authors. Part of the PulseX Quake port, GPL v2 or later (see LICENSE).

net_pc.c -- PulseX network driver table: loopback only (single player).

Replaces id's net_win.c, which also listed the Datagram driver over WinSock TCP/IP and IPX.
The table has the same shape as id's net_bsd.c minus the Datagram entry and the UDP LAN driver.
*/

#include "quakedef.h"
#include "net_loop.h"

net_driver_t net_drivers[MAX_NET_DRIVERS] =
{
	{
	"Loopback",
	false,
	Loop_Init,
	Loop_Listen,
	Loop_SearchForHosts,
	Loop_Connect,
	Loop_CheckNewConnections,
	Loop_GetMessage,
	Loop_SendMessage,
	Loop_SendUnreliableMessage,
	Loop_CanSendMessage,
	Loop_CanSendUnreliableMessage,
	Loop_Close,
	Loop_Shutdown
	}
};

int net_numdrivers = 1;

net_landriver_t	net_landrivers[MAX_NET_DRIVERS];
int net_numlandrivers = 0;
