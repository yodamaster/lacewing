
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

#include "Common.h"

#include "FrameReader.h"
#include "FrameBuilder.h"
#include "IDPool.h"

struct RelayServerInternal;

void ServerMessageHandler (void * Tag, unsigned char Type, char * Message, int Size);
void ServerTimerTick      (Lacewing::Timer &Timer);

struct RelayServerInternal
{
    ThreadTracker Threads;

    Lacewing::RelayServer &Server;
    Lacewing::Timer Timer;

    Lacewing::RelayServer::HandlerConnect           HandlerConnect;
    Lacewing::RelayServer::HandlerDisconnect        HandlerDisconnect;
    Lacewing::RelayServer::HandlerError             HandlerError;
    Lacewing::RelayServer::HandlerServerMessage     HandlerServerMessage;
    Lacewing::RelayServer::HandlerChannelMessage    HandlerChannelMessage;
    Lacewing::RelayServer::HandlerPeerMessage       HandlerPeerMessage;
    Lacewing::RelayServer::HandlerJoinChannel       HandlerJoinChannel;
    Lacewing::RelayServer::HandlerLeaveChannel      HandlerLeaveChannel;
    Lacewing::RelayServer::HandlerSetName           HandlerSetName;

    RelayServerInternal(Lacewing::RelayServer &_Server, Lacewing::EventPump &EventPump)
            : Server(_Server), Builder(false), Timer(EventPump)
    {
        HandlerConnect          = 0;
        HandlerDisconnect       = 0;
        HandlerError            = 0;
        HandlerServerMessage    = 0;
        HandlerChannelMessage   = 0;
        HandlerPeerMessage      = 0;
        HandlerJoinChannel      = 0;
        HandlerLeaveChannel     = 0;
        HandlerSetName          = 0;

        WelcomeMessage += Lacewing::Version();
    
        Timer.Tag = this;
        Timer.onTick(ServerTimerTick);
    }

    IDPool ClientIDs;
    IDPool ChannelIDs;

    struct Channel;

    struct Client
    {
        Lacewing::RelayServer::Client Public;

        Lacewing::Server::Client &Socket;
        RelayServerInternal                 &Server;

        Client(Lacewing::Server::Client &_Socket)
                : Server(*(RelayServerInternal *) Socket.Tag), Socket(_Socket),
                    UDPAddress(Socket.GetAddress())
        {
            Public.InternalTag    = this;
            Public.Tag            = 0;

            Reader.Tag            = this;
            Reader.MessageHandler = ServerMessageHandler;

            ID = Server.ClientIDs.Borrow();

            SentUDPWelcome = false;
            Handshook      = false;
            Ponged         = true;
        }

        ~Client()
        {
            Server.ClientIDs.Return(ID);  
        }

        FrameReader Reader;
        
        void MessageHandler (unsigned char Type, char * Message, int Size, bool Blasted);

        list <Channel *> Channels;

        string Name;

        unsigned short ID;
    
        Channel * ReadChannel(MessageReader &Reader);
    
        bool SentUDPWelcome;
        bool Handshook;
        bool Ponged;

        Lacewing::Address UDPAddress;

    };

    struct Channel
    {
        Lacewing::RelayServer::Channel Public;
        
        RelayServerInternal &Server;

        Channel(RelayServerInternal &_Server) : Server(_Server)
        {
            Public.InternalTag    = this;
            Public.Tag            = 0;

            ID = Server.ChannelIDs.Borrow();
        }

        ~Channel()
        {
            Server.ChannelIDs.Return(ID);
        }

        list <RelayServerInternal::Client *> Clients;

        string Name;
    
        unsigned short ID;

        bool Hidden;
        bool AutoClose;
    
        Client * ChannelMaster;
        
        void RemoveClient(Client &);

        Client * ReadPeer(MessageReader &Reader);

        void Close();
    };
    
    Backlog<Lacewing::Server::Client, Client>
        ClientBacklog;

    Backlog<RelayServerInternal, Channel>
        ChannelBacklog;

    FrameBuilder Builder;

    string WelcomeMessage;

    list<Channel *> Channels;

    void TimerTick()
    {
        Lacewing::Server &Socket = Server.Socket;
        list<RelayServerInternal::Client *> ToDisconnect;

        Builder.AddHeader(9, 0); /* Ping */
        
        for(void * ID = Socket.ClientLoop(); ID; ID = Socket.ClientLoop(ID))
        {
            RelayServerInternal::Client &Client = *(RelayServerInternal::Client *) Socket.ClientLoopIndex(ID).Tag;
            
            if(!Client.Ponged)
            {
                ToDisconnect.push_back(&Client);
                continue;
            }

            Client.Ponged = false;
            Builder.Send(Client.Socket, false);
        }

        Builder.FrameReset();

        for(list<RelayServerInternal::Client *>::iterator it = ToDisconnect.begin();
                it != ToDisconnect.end(); ++ it)
        {
            (*it)->Socket.Disconnect();
        }
    }
};

void ServerMessageHandler (void * Tag, unsigned char Type, char * Message, int Size)
{   ((RelayServerInternal::Client *) Tag)->MessageHandler(Type, Message, Size, false);
}

void ServerTimerTick (Lacewing::Timer &Timer)
{   ((RelayServerInternal *) Timer.Tag)->TimerTick();
}

RelayServerInternal::Channel * RelayServerInternal::Client::ReadChannel(MessageReader &Reader)
{
    int ChannelID = Reader.Get <unsigned short> ();

    if(Reader.Failed)
        return 0;

    for(list<RelayServerInternal::Channel *>::iterator it = Channels.begin(); it != Channels.end(); ++ it)
        if((*it)->ID == ChannelID)
            return *it;
     
    Reader.Failed = true;
    return 0;
}

RelayServerInternal::Client * RelayServerInternal::Channel::ReadPeer(MessageReader &Reader)
{
    int PeerID = Reader.Get <unsigned short> ();

    if(Reader.Failed)
        return 0;

    for(list<RelayServerInternal::Client *>::iterator it = Clients.begin(); it != Clients.end(); ++ it)
        if((*it)->ID == PeerID)
            return *it;
     
    Reader.Failed = true;
    return 0;
}

void HandlerConnect(Lacewing::Server &Server, Lacewing::Server::Client &ClientSocket)
{
    RelayServerInternal &Internal = *(RelayServerInternal *) Server.Tag;

    ClientSocket.Tag = &Internal;
    ClientSocket.Tag = &Internal.ClientBacklog.Borrow(ClientSocket);
}

void HandlerDisconnect(Lacewing::Server &Server, Lacewing::Server::Client &ClientSocket)
{
    RelayServerInternal &Internal        = *(RelayServerInternal *) Server.Tag;
    RelayServerInternal::Client &Client  = *(RelayServerInternal::Client *) ClientSocket.Tag;

    for(list<RelayServerInternal::Channel *>::iterator it = Client.Channels.begin();
            it != Client.Channels.end(); ++ it)
    {
        (*it)->RemoveClient(Client);
    }

    if(Client.Handshook && Internal.HandlerDisconnect)
        Internal.HandlerDisconnect(Internal.Server, Client.Public);

    Internal.ClientBacklog.Return(Client);
}

void HandlerReceive(Lacewing::Server &Server, Lacewing::Server::Client &ClientSocket, char * Data, int Size)
{
    RelayServerInternal &Internal = *(RelayServerInternal *) Server.Tag;
    RelayServerInternal::Client &Client   = *(RelayServerInternal::Client *) ClientSocket.Tag;

    Client.Reader.Process(Data, Size);
}

void HandlerError(Lacewing::Server &Server, Lacewing::Error &Error)
{
    RelayServerInternal &Internal = *(RelayServerInternal *) Server.Tag;

    Error.Add("Socket error");

    if(Internal.HandlerError)
        Internal.HandlerError(Internal.Server, Error);
}

void HandlerUDPReceive(Lacewing::UDP &UDP, Lacewing::Address &Address, char * Data, int Size)
{
    RelayServerInternal &Internal = *(RelayServerInternal *) UDP.Tag;

    if(Size < (sizeof(unsigned short) + 1))
        return;

    unsigned char Type = *(unsigned char  *) Data;
    unsigned short ID  = *(unsigned short *) (Data + 1);

    Data += sizeof(unsigned short) + 1;
    Size -= sizeof(unsigned short) + 1;

    Lacewing::Server &Socket = Internal.Server.Socket;

    for(void * Loop = Socket.ClientLoop(); Loop; Loop = Socket.ClientLoop(Loop))
    {
        RelayServerInternal::Client &Client = *(RelayServerInternal::Client *) Socket.ClientLoopIndex(Loop).Tag;

        if(Client.ID == ID)
        {
            if(Client.Socket.GetAddress().IP() != Address.IP())
                return;

            Client.UDPAddress.Port(Address.Port());
            Client.MessageHandler(Type, Data, Size, true);

            Socket.EndClientLoop(Loop);
            break;
        }
    }
}

void HandlerUDPError(Lacewing::UDP &UDP, Lacewing::Error &Error)
{
    RelayServerInternal &Internal = *(RelayServerInternal *) UDP.Tag;

    Error.Add("UDP socket error");

    if(Internal.HandlerError)
        Internal.HandlerError(Internal.Server, Error);
}

Lacewing::RelayServer::RelayServer(Lacewing::EventPump &EventPump) : Socket(EventPump), UDP(EventPump)
{
    LacewingInitialise();

    Socket.onConnect     (::HandlerConnect);
    Socket.onDisconnect  (::HandlerDisconnect);
    Socket.onReceive     (::HandlerReceive);
    Socket.onError       (::HandlerError);

    UDP.onReceive  (::HandlerUDPReceive);
    UDP.onError    (::HandlerUDPError);

    Socket.Tag = UDP.Tag = InternalTag = new RelayServerInternal(*this, EventPump);
    
    Socket.DisableNagling ();
}

Lacewing::RelayServer::~RelayServer()
{
    Unhost();

    delete ((RelayServerInternal *) InternalTag);
}

void Lacewing::RelayServer::Host(int Port)
{
    Lacewing::Filter Filter;
    Filter.LocalPort(Port);

    Host(Filter);
}

void Lacewing::RelayServer::Host(Lacewing::Filter &_Filter)
{
    Lacewing::Filter Filter(_Filter);

    if(!Filter.LocalPort())
        Filter.LocalPort(6121);

    Socket.Host (Filter, true);
    UDP.Host    (Filter);

    ((RelayServerInternal *) InternalTag)->Timer.Start(5000);
}

void Lacewing::RelayServer::Unhost()
{
    Socket.Unhost();
    UDP.Unhost();

    ((RelayServerInternal *) InternalTag)->Timer.Stop();
}

bool Lacewing::RelayServer::Hosting()
{
    return Socket.Hosting();
}

int Lacewing::RelayServer::Port()
{
    return Socket.Port();
}

void RelayServerInternal::Channel::Close()
{
    FrameBuilder &Builder = Server.Builder;

    /* Tell all the clients that they've left, and remove this channel from their channel lists. */

    Builder.AddHeader   (0, 0, false); /* Response */
    Builder.Add <unsigned char>   (3); /* LeaveChannel */
    Builder.Add <unsigned char>   (1); /* Success */
    Builder.Add <unsigned short> (ID);

    for(list<RelayServerInternal::Client *>::iterator it = Clients.begin();
            it != Clients.end(); ++ it)
    {
        RelayServerInternal::Client &Client = **it;
        Builder.Send(Client.Socket, false);

        for(list<RelayServerInternal::Channel *>::iterator it2 = Client.Channels.begin();
                it2 != Client.Channels.end(); ++ it)
        {
            if(*it2 == this)
            {
                Client.Channels.erase(it2);
                break;
            }
        }
    }

    Builder.FrameReset();

    
    /* Remove this channel from the channel list and return it to the backlog. */

    for(list<RelayServerInternal::Channel *>::iterator it = Server.Channels.begin();
            it != Server.Channels.end(); ++ it)
    {
        if((*it) == this)
        {
            Server.Channels.erase(it);
            break;
        }
    }
    
    Server.ChannelBacklog.Return(*this);
}

void RelayServerInternal::Channel::RemoveClient(RelayServerInternal::Client &Client)
{
    for(list<RelayServerInternal::Client *>::iterator it = Clients.begin();
            it != Clients.end(); ++ it)
    {
        if((*it) == &Client)
        {
            Clients.erase(it);
            break;
        }
    }

    if((!Clients.size()) || (ChannelMaster == &Client && AutoClose))
    {   
        Close();
        return;
    }

    if(ChannelMaster == &Client)
        ChannelMaster = 0;


    FrameBuilder &Builder = Server.Builder;

    /* Notify all the other peers that this client has left the channel */

    Builder.AddHeader (7, 0); /* Peer */
    
    Builder.Add <unsigned short> (ID);
    Builder.Add <unsigned short> (Client.ID);

    for(list<RelayServerInternal::Client *>::iterator it = Clients.begin();
            it != Clients.end(); ++ it)
    {
        Builder.Send((*it)->Socket, false);
    }

    Builder.FrameReset();
}

void RelayServerInternal::Client::MessageHandler(unsigned char Type, char * Message, int Size, bool Blasted)
{
    unsigned char MessageTypeID  = (Type >> 4);
    unsigned char Variant        = (Type << 4);

    Variant >>= 4;

    MessageReader Reader (Message, Size);
    FrameBuilder &Builder = Server.Builder;

    if(MessageTypeID != 0 && !Handshook)
    {
        Socket.Disconnect();
        return;
    }

    switch(MessageTypeID)
    {
        case 0: /* Request */
        {
            unsigned char RequestType = Reader.Get <unsigned char> ();

            if(Reader.Failed)
                break;

            if(RequestType != 0 && !Handshook)
            {
                Reader.Failed = true;
                break;
            }

            switch(RequestType)
            {
                case 0: /* Connect */
                {
                    const char * Version = Reader.GetRemaining ();

                    if(Reader.Failed)
                        break;

                    if(Handshook)
                    {
                        Reader.Failed = true;
                        break;
                    }

                    if(strcmp(Version, "revision 2"))
                    {
                        Builder.AddHeader        (0, 0);  /* Response */
                        Builder.Add <unsigned char> (0);  /* Connect */
                        Builder.Add <unsigned char> (0);  /* Failed */
                        Builder.Add ("Version mismatch", -1);

                        Builder.Send(Socket);

                        Reader.Failed = true;
                        break;
                    }

                    if(Server.HandlerConnect && !Server.HandlerConnect(Server.Server, Public))
                    {
                        Builder.AddHeader        (0, 0);  /* Response */
                        Builder.Add <unsigned char> (0);  /* Connect */
                        Builder.Add <unsigned char> (0);  /* Failed */
                        Builder.Add ("Connection refused by server", -1);

                        Builder.Send(Socket);

                        Reader.Failed = true;
                        break;
                    }

                    Handshook = true;

                    Builder.AddHeader          (0, 0);  /* Response */
                    Builder.Add <unsigned char>   (0);  /* Connect */
                    Builder.Add <unsigned char>   (1);  /* Success */
                    
                    Builder.Add <unsigned short> (ID);
                    Builder.Add (Server.WelcomeMessage);

                    Builder.Send(Socket);

                    break;
                }

                case 1: /* SetName */
                {
                    const char * Name = Reader.GetRemaining (false);

                    if(Reader.Failed)
                        break;

                    bool Taken = false;

                    for(list<RelayServerInternal::Channel *>::iterator it = Channels.begin();
                            it != Channels.end(); ++ it)
                    {
                        RelayServerInternal::Channel * Channel = *it;

                        for(list<RelayServerInternal::Client *>::iterator it2 = Channel->Clients.begin();
                                it2 != Channel->Clients.end(); ++ it2)
                        {
                            RelayServerInternal::Client * Client = *it2;

                            if(!stricmp(Client->Name.c_str(), Name))
                            {
                                Taken = true;
                                break;
                            }
                        }

                        if(Taken)
                            break;
                    }

                    if(Taken)
                    {
                        Builder.AddHeader        (0, 0);  /* Response */
                        Builder.Add <unsigned char> (1);  /* SetName */
                        Builder.Add <unsigned char> (0);  /* Failed */

                        Builder.Add <unsigned char> (strlen(Name));
                        Builder.Add (Name, -1);

                        Builder.Add ("Name already taken", -1);

                        Builder.Send(Socket);

                        break;
                    }

                    this->Name  = "";
                    this->Name += Name;

                    if(Server.HandlerSetName && !Server.HandlerSetName(Server.Server, Public, Name))
                    {
                        Builder.AddHeader        (0, 0);  /* Response */
                        Builder.Add <unsigned char> (1);  /* SetName */
                        Builder.Add <unsigned char> (0);  /* Failed */
                        
                        Builder.Add <unsigned char> (strlen(Name));
                        Builder.Add (Name, -1);

                        Builder.Add ("Name refused by server", -1);

                        Builder.Send(Socket);

                        break;
                    }

                    Builder.AddHeader        (0, 0);  /* Response */
                    Builder.Add <unsigned char> (1);  /* SetName */
                    Builder.Add <unsigned char> (1);  /* Success */
                
                    Builder.Add <unsigned char> (this->Name.length());
                    Builder.Add (this->Name);

                    Builder.Send(Socket);

                    for(list<RelayServerInternal::Channel *>::iterator it = Channels.begin();
                            it != Channels.end(); ++ it)
                    {
                        RelayServerInternal::Channel * Channel = *it;

                        for(list<RelayServerInternal::Client *>::iterator it2 = Channel->Clients.begin();
                                it2 != Channel->Clients.end(); ++ it2)
                        {
                            if(*it2 == this)
                                continue;

                            Builder.AddHeader (7, 0); /* Peer */
                            
                            Builder.Add <unsigned short> (Channel->ID);
                            Builder.Add <unsigned short> (ID);
                            Builder.Add <unsigned char>  (this == Channel->ChannelMaster ? 1 : 0);
                            Builder.Add (this->Name);

                            Builder.Send((*it2)->Socket);
                        }
                    }

                    break;
                }

                case 2: /* JoinChannel */
                {            
                    if(!this->Name.length())
                        Reader.Failed = true;

                    unsigned char Flags = Reader.Get <unsigned char> ();
                    char *        Name  = Reader.GetRemaining(false);
                    
                    if(Reader.Failed)
                        break;

                    RelayServerInternal::Channel * Channel = 0;

                    for(list<RelayServerInternal::Channel *>::iterator it = Server.Channels.begin();
                            it != Server.Channels.end(); ++ it)
                    {
                        if(!stricmp((*it)->Name.c_str(), Name))
                        {
                            Channel = *it;
                            break;
                        }
                    }
                    
                    if(Channel)
                    {
                        /* Joining an existing channel */

                        bool NameTaken = false;

                        for(list<RelayServerInternal::Client *>::iterator it2 = Channel->Clients.begin();
                                it2 != Channel->Clients.end(); ++ it2)
                        {
                            RelayServerInternal::Client * Client = *it2;

                            if(!stricmp(Client->Name.c_str(), Name))
                            {
                                NameTaken = true;
                                break;
                            }
                        }

                        if(NameTaken)
                        {
                            Builder.AddHeader        (0, 0);  /* Response */
                            Builder.Add <unsigned char> (2);  /* JoinChannel */
                            Builder.Add <unsigned char> (0);  /* Failed */

                            Builder.Add <unsigned char> (strlen(Name));
                            Builder.Add (Name, -1);

                            Builder.Add ("Name already taken", -1);

                            Builder.Send(Socket);

                            break;
                        }

                        if(Server.HandlerJoinChannel && !Server.HandlerJoinChannel(Server.Server, Public, Channel->Public))
                        {
                            Builder.AddHeader        (0, 0);  /* Response */
                            Builder.Add <unsigned char> (2);  /* JoinChannel */
                            Builder.Add <unsigned char> (0);  /* Failed */

                            Builder.Add <unsigned char> (Channel->Name.length());
                            Builder.Add (Channel->Name);

                            Builder.Add ("Join refused by server");

                            Builder.Send(Socket);
                            
                            break;
                        }
                    
                        Builder.AddHeader        (0, 0);  /* Response */
                        Builder.Add <unsigned char> (2);  /* JoinChannel */
                        Builder.Add <unsigned char> (1);  /* Success */
                        Builder.Add <unsigned char> (0);  /* Not the channel master */

                        Builder.Add <unsigned char> (Channel->Name.length());
                        Builder.Add (Channel->Name);

                        Builder.Add <unsigned short> (Channel->ID);
                        
                        for(list<RelayServerInternal::Client *>::iterator it = Channel->Clients.begin();
                                it != Channel->Clients.end(); ++ it)
                        {
                            RelayServerInternal::Client * Client = *it;

                            Builder.Add <unsigned short> (Client->ID);
                            Builder.Add <unsigned char>  (Channel->ChannelMaster == Client ? 1 : 0);
                            Builder.Add <unsigned char>  (Client->Name.length());
                            Builder.Add (Client->Name);
                        }

                        Builder.Send(Socket);


                        Builder.AddHeader (7, 0); /* Peer */
                        
                        Builder.Add <unsigned short> (Channel->ID);
                        Builder.Add <unsigned short> (ID);
                        Builder.Add <unsigned char>  (0);
                        Builder.Add (this->Name);

                        /* Notify the other clients on the channel that this client has joined */

                        for(list<RelayServerInternal::Client *>::iterator it = Channel->Clients.begin();
                                it != Channel->Clients.end(); ++ it)
                        {
                            Builder.Send((*it)->Socket, false);
                        }

                        Builder.FrameReset();


                        /* Add this client to the channel */

                        Channels.push_back (Channel);
                        Channel->Clients.push_back (this);

                        break;
                    }

                    /* Creating a new channel */

                    Channel = &Server.ChannelBacklog.Borrow(Server);

                    Channel->Name          += Name;
                    Channel->ChannelMaster =  this;
                    Channel->Hidden        =  (Flags & 1) != 0;
                    Channel->AutoClose     =  (Flags & 2) != 0;

                    if(Server.HandlerJoinChannel && !Server.HandlerJoinChannel(Server.Server, Public, Channel->Public))
                    {
                        Builder.AddHeader        (0, 0);  /* Response */
                        Builder.Add <unsigned char> (2);  /* JoinChannel */
                        Builder.Add <unsigned char> (0);  /* Failed */

                        Builder.Add <unsigned char> (Channel->Name.length());
                        Builder.Add (Channel->Name);

                        Builder.Add ("Join refused by server");

                        Builder.Send(Socket);
                        
                        Server.ChannelBacklog.Return(*Channel);
                        break;
                    }

                    Server.Channels.push_back (Channel);
                    Channels.push_back (Channel);
                    Channel->Clients.push_back (this);

                    Builder.AddHeader        (0, 0);  /* Response */
                    Builder.Add <unsigned char> (2);  /* JoinChannel */
                    Builder.Add <unsigned char> (1);  /* Success */
                    Builder.Add <unsigned char> (1);  /* Channel master */

                    Builder.Add <unsigned char> (Channel->Name.length());
                    Builder.Add (Channel->Name);

                    Builder.Add <unsigned short> (Channel->ID);

                    Builder.Send(Socket);

                    break;
                }

                case 3: /* LeaveChannel */
                {
                    RelayServerInternal::Channel * Channel = ReadChannel(Reader);

                    if(Reader.Failed)
                        break;

                    if(Server.HandlerLeaveChannel && !Server.HandlerLeaveChannel(Server.Server, Public, Channel->Public))
                    {
                        Builder.AddHeader         (0, 0);  /* Response */
                        Builder.Add <unsigned char>  (3);  /* LeaveChannel */
                        Builder.Add <unsigned char>  (0);  /* Failed */
                        Builder.Add <unsigned short> (Channel->ID);

                        Builder.Add ("Leave refused by server");

                        Builder.Send(Socket);

                        break;
                    }

                    for(list<RelayServerInternal::Channel *>::iterator it = Channels.begin(); it != Channels.end(); ++ it)
                    {
                        if(*it == Channel)
                        {
                            Channels.erase(it);
                            break;
                        }
                    } 

                    Builder.AddHeader         (0, 0);  /* Response */
                    Builder.Add <unsigned char>  (3);  /* LeaveChannel */
                    Builder.Add <unsigned char>  (1);  /* Success */
                    Builder.Add <unsigned short> (Channel->ID);

                    Builder.Send(Socket);

                    /* Do this last, because it might delete the channel */

                    Channel->RemoveClient(*this);

                    break;
                }

                case 4: /* ChannelList */

                    Builder.AddHeader        (0, 0);  /* Response */
                    Builder.Add <unsigned char> (4);  /* ChannelList */
                    Builder.Add <unsigned char> (1);  /* Success */

                    for(list<RelayServerInternal::Channel *>::iterator it = Server.Channels.begin();
                            it != Server.Channels.end(); ++ it)
                    {
                        RelayServerInternal::Channel &Channel = **it;

                        if(Channel.Hidden)
                            continue;

                        Builder.Add <unsigned short> (Channel.Clients.size());
                        Builder.Add <unsigned char>  (Channel.Name.length());
                        Builder.Add (Channel.Name);
                    }

                    Builder.Send(Socket);

                    break;

                default:
                    
                    Reader.Failed = true;
                    break;
            }

            break;
        }

        case 1: /* BinaryServerMessage */
        {
            unsigned char Subchannel = Reader.Get <unsigned char> ();
            
            char * Message;
            unsigned int Size;

            Reader.GetRemaining(Message, Size);
            
            if(Reader.Failed)
                break;

            if(Server.HandlerServerMessage)
                Server.HandlerServerMessage(Server.Server, Public, Blasted, Subchannel, Message, Size, Variant);

            break;
        }

        case 2: /* BinaryChannelMessage */
        {
            unsigned char Subchannel          = Reader.Get <unsigned char> ();
            RelayServerInternal::Channel * Channel = ReadChannel (Reader);
            
            char * Message;
            unsigned int Size;

            Reader.GetRemaining(Message, Size);
            
            if(Reader.Failed)
                break;

            if(Server.HandlerChannelMessage && !Server.HandlerChannelMessage(Server.Server, Public, Channel->Public,
                        Blasted, Subchannel, Message, Size, Variant))
            {
                break;
            }

            Builder.AddHeader (2, 0, Blasted); /* BinaryChannelMessage */
            
            Builder.Add <unsigned char>  (Subchannel);
            Builder.Add <unsigned short> (Channel->ID);
            Builder.Add <unsigned short> (ID);
            Builder.Add (Message, Size);

            for(list<RelayServerInternal::Client *>::iterator it = Channel->Clients.begin();
                    it != Channel->Clients.end(); ++ it)
            {
                if(*it == this)
                    continue;

                if(Blasted)
                    Builder.Send(Server.Server.UDP, (*it)->UDPAddress, false);
                else
                    Builder.Send((*it)->Socket, false);
            }

            Builder.FrameReset();

            break;
        }

        case 3: /* BinaryPeerMessage */
        {
            unsigned char Subchannel          = Reader.Get <unsigned char> ();
            RelayServerInternal::Channel * Channel = ReadChannel      (Reader);
            RelayServerInternal::Client  * Peer    = Channel->ReadPeer(Reader);

            if(Peer == this)
            {
                Reader.Failed = true;
                break;
            }

            char * Message;
            unsigned int Size;

            Reader.GetRemaining(Message, Size);
            
            if(Reader.Failed)
                break;

            if(Server.HandlerPeerMessage && !Server.HandlerPeerMessage(Server.Server, Public, Channel->Public,
                Peer->Public, Blasted, Subchannel, Message, Size, Variant))
            {
                break;
            }

            Builder.AddHeader (3, 0, Blasted); /* BinaryPeerMessage */
            
            Builder.Add <unsigned char>  (Subchannel);
            Builder.Add <unsigned short> (Channel->ID);
            Builder.Add <unsigned short> (ID);
            Builder.Add (Message, Size);

            if(Blasted)
                Builder.Send(Server.Server.UDP, Peer->UDPAddress, false);
            else
                Builder.Send(Peer->Socket);

            break;
        }
            
        case 4: /* ObjectServerMessage */

            break;
            
        case 5: /* ObjectChannelMessage */

            break;
            
        case 6: /* ObjectPeerMessage */

            break;
            
        case 7: /* UDPHello */

            if(!Blasted)
            {
                Reader.Failed = true;
                break;
            }

            if(!SentUDPWelcome)
            {
                Builder.AddHeader (8, 0); /* UDPWelcome */
                Builder.Send    (Socket);

                SentUDPWelcome = true;
            }

            break;
            
        case 8: /* ChannelMaster */

            break;

        case 9: /* Ping */

            Ponged = true;
            break;

        default:

            Reader.Failed = true;
            break;
    };

    if(Reader.Failed)
    {
        Socket.Disconnect();
    }
}

void Lacewing::RelayServer::Client::Send(int Subchannel, const char * Message, int Size, int Variant)
{
    RelayServerInternal::Client &Internal = *(RelayServerInternal::Client *) InternalTag;
    FrameBuilder &Builder = Internal.Server.Builder;

    Builder.AddHeader (1, Variant); /* BinaryServerMessage */
    
    Builder.Add <unsigned char> (Subchannel);
    Builder.Add (Message, Size);

    Builder.Send (Internal.Socket);
}

void Lacewing::RelayServer::Client::Blast(int Subchannel, const char * Message, int Size, int Variant)
{
    RelayServerInternal::Client &Internal = *(RelayServerInternal::Client *) InternalTag;
    FrameBuilder &Builder = Internal.Server.Builder;

    Builder.AddHeader (1, Variant, true); /* BinaryServerMessage */
    
    Builder.Add <unsigned char> (Subchannel);
    Builder.Add (Message, Size);

    Builder.Send (Internal.Server.Server.UDP, Internal.UDPAddress);
}

int Lacewing::RelayServer::Client::ID()
{
    return ((RelayServerInternal::Client *) InternalTag)->ID;
}

const char * Lacewing::RelayServer::Channel::Name()
{
    return ((RelayServerInternal::Channel *) InternalTag)->Name.c_str();
}

void Lacewing::RelayServer::Channel::Name(const char * Name)
{
    ((RelayServerInternal::Channel *) InternalTag)->Name  = "";
    ((RelayServerInternal::Channel *) InternalTag)->Name += Name;
}

bool Lacewing::RelayServer::Channel::Hidden()
{
    return ((RelayServerInternal::Channel *) InternalTag)->Hidden;
}

bool Lacewing::RelayServer::Channel::AutoCloseEnabled()
{
    return ((RelayServerInternal::Channel *) InternalTag)->AutoClose;
}

void Lacewing::RelayServer::SetWelcomeMessage(const char * Message)
{
    RelayServerInternal &Internal = *(RelayServerInternal *) InternalTag;

    Internal.WelcomeMessage  = "";
    Internal.WelcomeMessage += Message;
}

Lacewing::RelayServer::Client * Lacewing::RelayServer::Channel::ChannelMaster()
{
    RelayServerInternal::Client * Client = ((RelayServerInternal::Channel *) InternalTag)->ChannelMaster;

    return Client ? &Client->Public : 0;
}

void Lacewing::RelayServer::Channel::Close()
{
    ((RelayServerInternal::Channel *) InternalTag)->Close();
}

void Lacewing::RelayServer::Client::Disconnect()
{
    ((RelayServerInternal::Client *) InternalTag)->Socket.Disconnect();
}

Lacewing::Address &Lacewing::RelayServer::Client::GetAddress()
{
    return ((RelayServerInternal::Client *) InternalTag)->Socket.GetAddress();
}

const char * Lacewing::RelayServer::Client::Name()
{
    return ((RelayServerInternal::Client *) InternalTag)->Name.c_str();
}

void Lacewing::RelayServer::Client::Name(const char * Name)
{
    ((RelayServerInternal::Client *) InternalTag)->Name  = "";
    ((RelayServerInternal::Client *) InternalTag)->Name += Name;
}

void * Lacewing::RelayServer::ClientLoop(void * ID)
{
    return Socket.ClientLoop(ID);
}

Lacewing::RelayServer::Client &Lacewing::RelayServer::ClientLoopIndex(void * ID)
{
    return ((RelayServerInternal::Client *) Socket.ClientLoopIndex(ID).Tag)->Public;
}

void Lacewing::RelayServer::EndClientLoop(void * ID)
{
    Socket.EndClientLoop(ID);
}

int Lacewing::RelayServer::ChannelCount()
{
    return ((RelayServerInternal *) InternalTag)->Channels.size();
}

int Lacewing::RelayServer::Channel::ClientCount()
{
    return ((RelayServerInternal::Channel *) InternalTag)->Clients.size();
}

int Lacewing::RelayServer::Client::ChannelCount()
{
    return ((RelayServerInternal::Client *) InternalTag)->Channels.size();
}


Looper(A, RelayServer, Channel, RelayServerInternal, Channels, ((void) 0), 0,
        list<RelayServerInternal::Channel *>::iterator, Lacewing::RelayServer::Channel &, ->Public);

Looper(B, RelayServer::Client, Channel, RelayServerInternal::Client, Channels, ((void) 0), 0,
        list<RelayServerInternal::Channel *>::iterator, Lacewing::RelayServer::Channel &, ->Public);

Looper(A, RelayServer::Channel, Client, RelayServerInternal::Channel, Clients, ((void) 0), 0,
        list<RelayServerInternal::Client *>::iterator, Lacewing::RelayServer::Client &, ->Public);

AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, Connect)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, Disconnect)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, Error)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, ServerMessage)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, ChannelMessage)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, PeerMessage)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, JoinChannel)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, LeaveChannel)
AutoHandlerFunctions(Lacewing::RelayServer, RelayServerInternal, SetName)

