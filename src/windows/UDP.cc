
/*
    Copyright (C) 2011 James McLaughlin

    This file is part of Lacewing.

    Lacewing is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Lacewing is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with Lacewing.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "../Common.h"

const int IdealPendingReceiveCount = 16;

namespace OverlappedType
{
    enum Type
    {
        Send,
        Receive,
    };
}

struct UDPInternal;

struct UDPOverlapped
{
    OVERLAPPED Overlapped;

    OverlappedType::Type Type;
    void * Tag;

    UDPOverlapped(UDPInternal &)
    {
       memset(&Overlapped, 0, sizeof(OVERLAPPED));
    }
};

struct UDPReceiveInformation
{
    char Buffer[1024 * 32];
    WSABUF WinsockBuffer;

    UDPReceiveInformation(UDPInternal &)
    {
        WinsockBuffer.buf = Buffer;
        WinsockBuffer.len = sizeof(Buffer);

        FromLength = sizeof(sockaddr_in);
    }

    sockaddr_in From;
    int FromLength;
};

struct UDPInternal
{
    EventPumpInternal &EventPump;

    Lacewing::UDP &Public;
    
    UDPInternal(Lacewing::UDP &_Public, EventPumpInternal &_EventPump)
            : Public(_Public), EventPump(_EventPump)
    {
        RemoteIP       = 0;
        ReceivesPosted = 0;

        HandlerReceive = 0;
        HandlerError   = 0;

        Socket = SOCKET_ERROR;
    }

    Lacewing::UDP::HandlerReceive  HandlerReceive;
    Lacewing::UDP::HandlerError    HandlerError;

    int RemoteIP;
    int Port;

    SOCKET Socket;

    lw_i64 BytesSent;
    lw_i64 BytesReceived;

    Backlog<UDPInternal, UDPOverlapped>
        OverlappedBacklog;

    Backlog<UDPInternal, UDPReceiveInformation>
        ReceiveInformationBacklog;

    volatile long ReceivesPosted;

    void PostReceives()
    {
        while(ReceivesPosted < IdealPendingReceiveCount)
        {
            UDPReceiveInformation &ReceiveInformation = ReceiveInformationBacklog.Borrow(*this);
            UDPOverlapped &Overlapped = OverlappedBacklog.Borrow(*this);

            Overlapped.Type = OverlappedType::Receive;
            Overlapped.Tag = &ReceiveInformation;

            DWORD Flags = 0;

            if(WSARecvFrom(Socket, &ReceiveInformation.WinsockBuffer,
                            1, 0, &Flags, (sockaddr *) &ReceiveInformation.From, &ReceiveInformation.FromLength,
                            (OVERLAPPED *) &Overlapped, 0) == SOCKET_ERROR)
            {
                int Error = WSAGetLastError();

                if(Error != WSA_IO_PENDING)
                    break;
            }

            InterlockedIncrement(&ReceivesPosted);
        }
    }

};

void UDPSocketCompletion(UDPInternal &Internal, UDPOverlapped &Overlapped, unsigned int BytesTransferred, int Error)
{
    switch(Overlapped.Type)
    {
        case OverlappedType::Send:
        {
            Internal.BytesSent += BytesTransferred;
            break;
        }

        case OverlappedType::Receive:
        {
            Internal.BytesReceived += BytesTransferred;

            UDPReceiveInformation &ReceiveInformation = *(UDPReceiveInformation *) Overlapped.Tag;

            if(Internal.RemoteIP && ReceiveInformation.From.sin_addr.s_addr != Internal.RemoteIP)
                break;

            Lacewing::Address Address(ReceiveInformation.From.sin_addr.s_addr, ntohs(ReceiveInformation.From.sin_port));

            ReceiveInformation.Buffer[BytesTransferred] = 0;

            if(Internal.HandlerReceive)
                Internal.HandlerReceive(Internal.Public, Address, ReceiveInformation.Buffer, BytesTransferred);

            Internal.ReceiveInformationBacklog.Return(ReceiveInformation);

            InterlockedDecrement(&Internal.ReceivesPosted);
            Internal.PostReceives();

            break;
        }
    };

    Internal.OverlappedBacklog.Return(Overlapped);
}

void Lacewing::UDP::Host(int Port)
{
    Lacewing::Filter Filter;
    Filter.LocalPort(Port);

    Host(Filter);
}

void Lacewing::UDP::Host(Lacewing::Address &Address)
{
    Lacewing::Filter Filter;
    Filter.RemoteAddress(Address);

    Host(Filter);
}

void Lacewing::UDP::Host(Lacewing::Filter &Filter)
{
    Unhost();

    UDPInternal &Internal = *((UDPInternal *) InternalTag);

    if(Internal.Socket != SOCKET_ERROR)
    {
        Lacewing::Error Error;
        Error.Add("Already hosting");
        
        if(Internal.HandlerError)
            Internal.HandlerError(*this, Error);

        return;    
    }

    Internal.Socket   = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, 0, 0, WSA_FLAG_OVERLAPPED);
    Internal.RemoteIP = Filter.RemoteAddress().IP();

    Internal.EventPump.Add((HANDLE) Internal.Socket, &Internal, UDPSocketCompletion);

    sockaddr_in SocketAddress;
    memset(&SocketAddress, 0, sizeof(Address));

    SocketAddress.sin_family = AF_INET;
    SocketAddress.sin_port = htons(Filter.LocalPort() ? Filter.LocalPort() : 0);
    SocketAddress.sin_addr.s_addr = Filter.LocalIP() ? Filter.LocalIP() : INADDR_ANY;

    if(bind(Internal.Socket, (sockaddr *) &SocketAddress, sizeof(sockaddr_in)) == SOCKET_ERROR)
    {
        Lacewing::Error Error;
        
        Error.Add(WSAGetLastError());
        Error.Add("Error binding port");

        if(Internal.HandlerError)
            Internal.HandlerError(*this, Error);

        return;
    }

    socklen_t AddressLength = sizeof(sockaddr_in);
    getsockname(Internal.Socket, (sockaddr *) &SocketAddress, &AddressLength);

    Internal.Port = ntohs(SocketAddress.sin_port);
    Internal.PostReceives();
}

void Lacewing::UDP::Unhost()
{
    UDPInternal &Internal = *((UDPInternal *) InternalTag);

    LacewingCloseSocket(Internal.Socket);
    Internal.Socket = SOCKET_ERROR;
}

Lacewing::UDP::UDP(Lacewing::EventPump &EventPump)
{
    LacewingInitialise();  
    InternalTag = new UDPInternal(*this, *(EventPumpInternal *) EventPump.InternalTag);
}

Lacewing::UDP::~UDP()
{
    delete ((UDPInternal *) InternalTag);
}

void Lacewing::UDP::Send(Lacewing::Address &Address, const char * Data, int Size)
{
    UDPInternal &Internal = *(UDPInternal *) InternalTag;

    if(!Address.Ready())
    {
        Lacewing::Error Error;

        Error.Add("The address object passed to Send() wasn't ready");        
        Error.Add("Error sending");

        if(Internal.HandlerError)
            Internal.HandlerError(Internal.Public, Error);

        return;
    }

    if(Size == -1)
        Size = strlen(Data);

    WSABUF Buffer = { Size, (CHAR *) Data };

    UDPOverlapped &Overlapped = Internal.OverlappedBacklog.Borrow(Internal);

    Overlapped.Type = OverlappedType::Send;
    Overlapped.Tag  = 0;

    sockaddr_in To;
    GetSockaddr(Address, To);

    if(WSASendTo(Internal.Socket, &Buffer, 1, 0, 0, (sockaddr *) &To, sizeof(sockaddr_in),
                    (OVERLAPPED *) &Overlapped, 0) == SOCKET_ERROR)
    {
        int Code = WSAGetLastError();

        if(Code != WSA_IO_PENDING)
        {
            Lacewing::Error Error;

            Error.Add(Code);            
            Error.Add("Error sending");

            if(Internal.HandlerError)
                Internal.HandlerError(*this, Error);

            return;
        }
    }
}

lw_i64 Lacewing::UDP::BytesReceived()
{
    return ((UDPInternal *) InternalTag)->BytesReceived;
}

lw_i64 Lacewing::UDP::BytesSent()
{
    return ((UDPInternal *) InternalTag)->BytesSent;
}

AutoHandlerFunctions(Lacewing::UDP, UDPInternal, Error)
AutoHandlerFunctions(Lacewing::UDP, UDPInternal, Receive)

