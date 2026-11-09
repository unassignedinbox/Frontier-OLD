/*==========================================================================================================================================
                                                          NETWORKINGEXTENSION.CPP
==========================================================================================================================================*/
// 🧩 Replication of WorldSpace records as their level TOML, the messages' wire form and the loopback transport.

#include "Frontier/Extensions/Networking/NetworkingExtension.h"

#include "Frontier/Engine/Scene/WorldSpace.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace Frontier
{

namespace
{

constexpr size_t HeaderBytes = 24;

void WriteNumber(std::string& Bytes, uint64_t Value, size_t Width)
{
    for (size_t Index = 0; Index < Width; ++Index) Bytes.push_back(static_cast<char>((Value >> (8 * Index)) & 0xffu));
}

uint64_t ReadNumber(std::string_view Bytes, size_t Offset, size_t Width)
{
    uint64_t Value = 0;
    for (size_t Index = 0; Index < Width; ++Index) Value |= static_cast<uint64_t>(static_cast<unsigned char>(Bytes[Offset + Index])) << (8 * Index);
    return Value;
}

std::string Print(const toml::table& Table)
{
    std::ostringstream Out;
    Out << Table;
    return Out.str();
}

} // namespace

//------------------------------------------------------------------------------------------------------------------------
//                                                    WIRE FORM
//------------------------------------------------------------------------------------------------------------------------

std::string EncodeNetworkMessage(const NetworkMessage& Message)
{
    std::string Bytes;
    Bytes.reserve(HeaderBytes + Message.Payload.size());
    WriteNumber(Bytes, Message.From, 4);
    WriteNumber(Bytes, Message.To, 4);
    WriteNumber(Bytes, static_cast<uint64_t>(Message.Topic), 4);
    WriteNumber(Bytes, Message.Sequence, 8);
    WriteNumber(Bytes, Message.Payload.size(), 4);
    Bytes += Message.Payload;
    return Bytes;
}

bool DecodeNetworkMessage(std::string_view Bytes, NetworkMessage& Message)
{
    if (Bytes.size() < HeaderBytes) return false;
    const uint64_t Length = ReadNumber(Bytes, 20, 4);
    const uint64_t Topic  = ReadNumber(Bytes, 8, 4);
    if (Bytes.size() != HeaderBytes + Length || Topic > static_cast<uint64_t>(NetworkTopic::Farewell)) return false;
    Message.From     = static_cast<uint32_t>(ReadNumber(Bytes, 0, 4));
    Message.To       = static_cast<uint32_t>(ReadNumber(Bytes, 4, 4));
    Message.Topic    = static_cast<NetworkTopic>(Topic);
    Message.Sequence = ReadNumber(Bytes, 12, 8);
    Message.Payload  = std::string(Bytes.substr(HeaderBytes));
    return true;
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    LOOPBACK
//------------------------------------------------------------------------------------------------------------------------

void LoopbackTransport::Connect(LoopbackTransport& First, LoopbackTransport& Second) noexcept
{
    First.Partner  = &Second;
    Second.Partner = &First;
}

bool LoopbackTransport::Send(const NetworkMessage& Message)
{
    if (Partner == nullptr) return false;
    Partner->Packets.push_back(EncodeNetworkMessage(Message));
    return true;
}

std::vector<NetworkMessage> LoopbackTransport::Receive()
{
    std::vector<NetworkMessage> Messages;
    for (const std::string& Packet : Packets)
    {
        NetworkMessage Message;
        if (DecodeNetworkMessage(Packet, Message)) Messages.push_back(std::move(Message));
    }
    Packets.clear();
    return Messages;
}

//------------------------------------------------------------------------------------------------------------------------
//                                                    REPLICATION
//------------------------------------------------------------------------------------------------------------------------

void NetworkingExtension::Greet()
{
    const NetworkPeer* Local = Space.QueryLocalPeer();
    if (Local == nullptr) return;
    Space.Post(NetworkTopic::Greeting, Print(toml::table{ { "name", Local->Name }, { "role", Local->Role == NetworkRole::Host ? "host" : "client" } }));
}

size_t NetworkingExtension::Replicate(const WorldSpace& World)
{
    size_t                       Posted = 0;
    std::unordered_set<uint64_t> Present;
    for (const auto& [Type, Category] : World.EnumerateRegisteredTypes())
    {
        for (const SceneRecordBase* Record : World.FindAllByType(Type))
        {
            const uint64_t Token = Record->QueryToken().Value;
            Present.insert(Token);
            toml::table Configuration;
            Record->SerializeConfiguration(Configuration);
            std::string Payload = Print(toml::table{ { "token", static_cast<int64_t>(Token) },
                                                     { "type", Record->QueryTypeIdentifier() },
                                                     { "name", Record->QueryRecordName() },
                                                     { "configuration", std::move(Configuration) } });
            std::string& Last = Sent[Token];
            if (Last == Payload) continue;
            Last = Payload;
            Space.Post(NetworkTopic::RecordConfiguration, std::move(Payload));
            ++Posted;
        }
    }
    for (auto Entry = Sent.begin(); Entry != Sent.end();)
    {
        if (Present.count(Entry->first) == 0)
        {
            Space.Post(NetworkTopic::RecordRemoval, Print(toml::table{ { "token", static_cast<int64_t>(Entry->first) } }));
            ++Posted;
            Entry = Sent.erase(Entry);
        }
        else ++Entry;
    }
    return Posted;
}

void NetworkingExtension::Exchange()
{
    for (const NetworkMessage& Message : Space.TakeOutgoing()) (void)Transport.Send(Message);
    for (NetworkMessage& Message : Transport.Receive()) Space.Deliver(std::move(Message));
}

size_t NetworkingExtension::Absorb(WorldSpace& World)
{
    size_t Applied = 0;
    for (const NetworkMessage& Message : Space.TakeIncoming())
    {
        toml::table Document;
        try
        {
            Document = toml::parse(Message.Payload);
        }
        catch (const toml::parse_error&)
        {
            continue;
        }
        if (Message.Topic == NetworkTopic::Greeting)
        {
            const std::string Name = Document["name"].value<std::string>().value_or("peer");
            const bool        Known = std::any_of(Space.QueryPeers().begin(), Space.QueryPeers().end(), [&](const NetworkPeer& Peer) { return Peer.Name == Name; });
            if (!Known) (void)Space.AdmitPeer(Name, Document["role"].value<std::string>().value_or("client") == "host" ? NetworkRole::Host : NetworkRole::Client);
            ++Applied;
            continue;
        }
        const uint64_t Token = static_cast<uint64_t>(Document["token"].value<int64_t>().value_or(0));
        if (Message.Topic == NetworkTopic::RecordRemoval)
        {
            if (const auto Found = Matched.find(Token); Found != Matched.end())
            {
                (void)World.DestroyRecord(Found->second);
                Matched.erase(Found);
                ++Applied;
            }
            continue;
        }
        if (Message.Topic != NetworkTopic::RecordConfiguration) continue;
        const std::string  Type          = Document["type"].value<std::string>().value_or("");
        const std::string  Name          = Document["name"].value<std::string>().value_or("");
        const toml::table* Configuration = Document["configuration"].as_table();
        if (Type.empty() || Configuration == nullptr) continue;
        RecordToken Local{};
        if (const auto Found = Matched.find(Token); Found != Matched.end()) Local = Found->second;
        if (!Local.IsValid() || World.FindRecord(Local) == nullptr)
        {
            Local = RecordToken{};
            for (const SceneRecordBase* Candidate : World.FindAllByType(Type))
            {
                const bool Taken = std::any_of(Matched.begin(), Matched.end(), [&](const auto& Entry) { return Entry.second == Candidate->QueryToken(); });
                if (!Taken && Candidate->QueryRecordName() == Name)
                {
                    Local = Candidate->QueryToken();
                    break;
                }
            }
            if (!Local.IsValid()) Local = World.ConstructRecord(Type, Name);
            if (!Local.IsValid()) continue;
            Matched[Token] = Local;
        }
        if (SceneRecordBase* Record = World.FindRecord(Local))
        {
            Record->DeserializeConfiguration(*Configuration);
            ++Applied;
        }
    }
    return Applied;
}

} // namespace Frontier
