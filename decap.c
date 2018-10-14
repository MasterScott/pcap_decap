//---------------------------------------------------------
//
// fmadio pcap de-encapsuation utility
//
// Copyright (C) 2018 fmad engineering llc aaron foo 
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//
// 
// automatic packet de-encapsulation 
//
//---------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/ioctl.h>
#include <linux/tcp.h>

#include "fTypes.h"
#include "fNetwork.h"
#include "decap.h"

bool g_DecapDump		= false;
bool g_DecapVerbose		= false;
bool g_DecapMetaMako	= false;
bool g_DecapIxia		= false;
bool g_DecapArista		= false;

//---------------------------------------------------------------------------------------------


void fDecap_Arista_Open		(int argc, char* argv[]);
void fDecap_ERSPAN3_Open	(int argc, char* argv[]);
void fDecap_MetaMako_Open	(int argc, char* argv[]);
void fDecap_Ixia_Open		(int argc, char* argv[]);

void fDecap_Arista_Close	(void);
void fDecap_ERSPAN3_Close	(void);
void fDecap_MetaMako_Close	(void);
void fDecap_Ixia_Close		(void);

u16 fDecap_ERSPAN3_Unpack	(u64 TS, fEther_t** pEther, u8** pPayload, u32* pPayloadLength, u32* MetaPort, u64* MetaTS, u32* MetaFCS);
u16 fDecap_MetaMako_Unpack	(u64 PCAPTS, fEther_t** pEther, u8** pPayload, u32* pPayloadLength, u32* pMetaPort, u64* pMetaTS, u32* pMetaFCS);
u16 fDecap_Ixia_Unpack		(u64 PCAPTS, fEther_t** pEther, u8** pPayload, u32* pPayloadLength, u32* pMetaPort, u64* pMetaTS, u32* pMetaFCS);
u16 fDecap_Arista_Unpack	(u64 PCAPTS, fEther_t** pEther, u8** pPayload, u32* pPayloadLength, u32* pMetaPort, u64* pMetaTS, u32* pMetaFCS);

//---------------------------------------------------------------------------------------------
/*
void fDecap_Mode(u32 Mode)
{
	// reset all
	g_DecapMetaMako = false;
	g_DecapIxia 	= false;
	g_DecapArista 	= false;

	switch (Mode)
	{
	case FNIC_PACKET_TSMODE_NIC:
		break;

	case FNIC_PACKET_TSMODE_MMAKO:
		g_DecapMetaMako = true;
		break;

	case FNIC_PACKET_TSMODE_IXIA:
		g_DecapIxia 	= true;
		break;

	case FNIC_PACKET_TSMODE_ARISTA:
		g_DecapArista 	= true;
		break;
	}
}
*/

//---------------------------------------------------------------------------------------------

void fDecap_Open(int argc, char* argv[])
{
	// packet meta data is explicit 
	if (g_DecapArista) 		fDecap_Arista_Open		(argc, argv);
	if (g_DecapMetaMako) 	fDecap_MetaMako_Open	(argc, argv);
	if (g_DecapIxia) 		fDecap_Ixia_Open		(argc, argv);

	// protocol implicit in the payload 
	fDecap_ERSPAN3_Open(argc, argv);
}

//---------------------------------------------------------------------------------------------

void fDecap_Close(void)
{
	// packet meta data is explicit 
	if (g_DecapArista) 		fDecap_Arista_Close	();
	if (g_DecapMetaMako) 	fDecap_MetaMako_Close	();
	if (g_DecapIxia) 		fDecap_Ixia_Close		();

	// protocol implicit in the payload 
	fDecap_ERSPAN3_Close();
}

//---------------------------------------------------------------------------------------------
// de-encapsulate a packet
u16 fDecap_Packet(	u64 PCAPTS,
					struct fEther_t** pEther, 

					u8** pPayload, 
					u32* pPayloadLength,

					u32* pMetaPort, 
					u64* pMetaTS, 
					u32* pMetaFCS)
{
	fEther_t* Ether = pEther[0];

	// get first level ether protocol 
	u16 EtherProto = swap16(Ether->Proto);
	//fprintf(stderr, "decap: %04x\n", EtherProto); 

	u8* Payload 		= pPayload[0];
	u32 PayloadLength 	= pPayloadLength[0];

	// vlan decode
	if (EtherProto == ETHER_PROTO_VLAN)
	{
		VLANTag_t* Header 	= (VLANTag_t*)(Ether+1);
		u16* Proto 			= (u16*)(Header + 1);

		// update to the acutal proto / ipv4 header
		EtherProto 			= swap16(Proto[0]);
		Payload 			= (u8*)(Proto + 1);

		// VNTag unpack (BME) 
		if (EtherProto == ETHER_PROTO_VNTAG)
		{
			VNTag_t* Header = (VNTag_t*)(Proto+1);
			Proto 			= (u16*)(Header + 1);

			// update to the acutal proto / ipv4 header
			EtherProto 		= swap16(Proto[0]);
			Payload 		= (u8*)(Proto + 1);
		}

		// is it double tagged ? 
		if (EtherProto == ETHER_PROTO_VLAN)
		{
			Header 			= (VLANTag_t*)(Proto+1);
			Proto 			= (u16*)(Header + 1);

			// update to the acutal proto / ipv4 header
			EtherProto 		= swap16(Proto[0]);
			Payload 		= (u8*)(Proto + 1);
		}
	}

	// mpls decode 
	if (EtherProto == ETHER_PROTO_MPLS)
	{
		// find bottom of stack
		MPLSHeader_t* Header0 	= (MPLSHeader_t*)(Ether+1);
		MPLSHeader_t* Header1 	= Header0 + 1; 
		MPLSHeader_t* Header2 	= Header0 + 2; 
		MPLSHeader_t* Header3 	= Header0 + 3; 

		// assume its always IPv4 (tho could be IPv6)
		EtherProto 			= ETHER_PROTO_IPV4;

		// single tag
		if (Header0->BOS)
		{
			Payload 			= (u8*)(Header0 + 1);
		}
		// dobuble tag
		else if (Header1->BOS)
		{
			Payload 			= (u8*)(Header1 + 1);
		}
		// tripple tag
		else if (Header2->BOS)
		{
			Payload 			= (u8*)(Header2 + 1);
		}
		// quad tag
		else if (Header3->BOS)
		{
			Payload 			= (u8*)(Header3 + 1);
		}
	}

	// VNTag unpack (BME) 
	if (EtherProto == ETHER_PROTO_VNTAG)
	{
		VNTag_t* Header 	= (VNTag_t*)(Ether+1);
		u16* Proto 			= (u16*)(Header + 1);

		// update to the acutal proto / ipv4 header
		EtherProto 			= swap16(Proto[0]);
		Payload 			= (u8*)(Proto + 1);
	}

	// set new Ether header (if any)
	pEther[0] 			= Ether;

	// set new IP header
	pPayload[0] 		= Payload;
	pPayloadLength[0]	= PayloadLength;

	// GRE/ERSPAN
	if (EtherProto == ETHER_PROTO_IPV4)
	{
		IPv4Header_t* IPv4Header = (IPv4Header_t*)Payload;
		if (IPv4Header->Proto == IPv4_PROTO_GRE)
		{
			GREHeader_t* GRE = (GREHeader_t*)((u8*)IPv4Header + IPv4Header->HLen*4);

			u32 GREProto = swap16(GRE->Proto);
			switch(GREProto)
			{
			case GRE_PROTO_ERSPAN2: 
				trace("ERSPANv2 not supported\n");
				break;
				
			case GRE_PROTO_ERSPAN3: return fDecap_ERSPAN3_Unpack(PCAPTS, pEther, pPayload, pPayloadLength, pMetaPort, pMetaTS, pMetaFCS);
			default:
				trace("GRE Proto unsuported format: %x\n", GREProto);
				break;
			}
		}
	}

	// extract data from footers 
	if (g_DecapMetaMako)
	{
		fDecap_MetaMako_Unpack(PCAPTS, pEther, pPayload, pPayloadLength, pMetaPort, pMetaTS, pMetaFCS);
	}
	if (g_DecapIxia)
	{
		fDecap_Ixia_Unpack(PCAPTS, pEther, pPayload, pPayloadLength, pMetaPort, pMetaTS, pMetaFCS);
	}
	if (g_DecapArista)
	{
		fDecap_Arista_Unpack(PCAPTS, pEther, pPayload, pPayloadLength, pMetaPort, pMetaTS, pMetaFCS);
	}

	// update
	return EtherProto;
}