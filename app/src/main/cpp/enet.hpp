#pragma once

#include <enet/enet.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace gameenet {

    constexpr std::size_t kRoomCodeLength = 8;
    constexpr std::size_t kMaxPlayersPerRoom = 4;
    constexpr std::size_t kMaxNameLength = 24;

    enum class PacketType : std::uint8_t {
        JoinRequest = 1,
        JoinResponse = 2,
        InputState = 3,
        Snapshot = 4,
        ChatMessage = 5,
    };

    enum class JoinRejectReason : std::uint8_t {
        None = 0,
        InvalidRoomCode = 1,
        RoomFull = 2,
        ServerError = 3,
    };

    struct PlayerView {
        std::uint32_t clientId = 0;
        std::string name;
        std::uint8_t colorIndex = 0;
        float x = 0.0f;
        float y = 0.0f;
        bool moving = false;
        bool facingLeft = false;
        bool active = false;
    };

    struct JoinInfo {
        bool accepted = false;
        std::uint32_t clientId = 0;
        std::uint8_t colorIndex = 0;
        std::string roomCode;
        std::string message;
        std::vector<PlayerView> players;
    };

#pragma pack(push, 1)
    struct JoinRequestPacket {
        std::uint8_t type = 0;
        char roomCode[kRoomCodeLength + 1]{};
        char name[kMaxNameLength + 1]{};
    };

    struct JoinResponsePacket {
        std::uint8_t type = 0;
        std::uint8_t accepted = 0;
        std::uint8_t reason = 0;
        std::uint8_t playerCount = 0;
        std::uint32_t clientId = 0;
        std::uint8_t colorIndex = 0;
        char roomCode[kRoomCodeLength + 1]{};
        char message[64]{};
    };

    struct InputPacket {
        std::uint8_t type = 0;
        std::int8_t moveX = 0;
        std::int8_t moveY = 0;
        std::uint8_t moving = 0;
        std::uint8_t facingLeft = 0;
    };

    struct SnapshotPlayerPacket {
        std::uint32_t clientId = 0;
        std::uint8_t colorIndex = 0;
        std::uint8_t moving = 0;
        std::uint8_t facingLeft = 0;
        std::uint8_t active = 0;
        float x = 0.0f;
        float y = 0.0f;
        char name[kMaxNameLength + 1]{};
    };

    struct SnapshotPacket {
        std::uint8_t type = 0;
        char roomCode[kRoomCodeLength + 1]{};
        std::uint8_t playerCount = 0;
        std::uint32_t serverTick = 0;
        SnapshotPlayerPacket players[kMaxPlayersPerRoom]{};
    };

    struct ChatMessagePacket {
        std::uint8_t type = 0;
        std::uint32_t clientId = 0;
        std::uint8_t colorIndex = 0;
        char name[kMaxNameLength + 1]{};
        char message[96]{};
    };
#pragma pack(pop)

    struct ChatLine {
        std::uint32_t clientId = 0;
        std::uint8_t colorIndex = 0;
        std::string name;
        std::string message;
    };

    class ClientNet {
    public:
        ClientNet() = default;
        ~ClientNet();

        bool connect(const std::string& host, std::uint16_t port, const std::string& roomCode, const std::string& name, std::string* errorOut = nullptr);
        void disconnect();
        void sendInput(int moveX, int moveY, bool moving, bool facingLeft);
        void sendChatMessage(const std::string& message);
        bool pump(std::string* errorOut = nullptr);

        bool connected() const { return connected_; }
        bool connecting() const { return connecting_; }
        const JoinInfo& joinInfo() const { return joinInfo_; }
        const std::vector<PlayerView>& players() const { return players_; }
        const std::vector<ChatLine>& chatLog() const { return chatLog_; }
        int pingMs() const;
        std::string connectionSummary() const;
        std::string lastError() const { return lastError_; }

    private:
        void clear();
        void handleJoinResponse(const JoinResponsePacket& packet);
        void handleSnapshot(const SnapshotPacket& packet);
        void handleChatMessage(const ChatMessagePacket& packet);
        void sendPendingJoinRequest();

        ENetHost* host_ = nullptr;
        ENetPeer* peer_ = nullptr;
        bool enetInitialized_ = false;
        bool connecting_ = false;
        bool connected_ = false;
        bool joinRequestSent_ = false;
        std::string pendingRoomCode_;
        std::string pendingName_;
        std::string localName_;
        std::string lastError_;
        JoinInfo joinInfo_;
        std::vector<PlayerView> players_;
        std::vector<ChatLine> chatLog_;
    };

    class ServerApp {
    public:
        ~ServerApp();

        bool start(const std::string& bindHost, std::uint16_t port, std::string* errorOut = nullptr);
        void run();
        void stop();

    private:
        struct RoomPlayer {
            ENetPeer* peer = nullptr;
            std::uint32_t clientId = 0;
            std::uint8_t colorIndex = 0;
            std::string name;
            float x = 0.0f;
            float y = 0.0f;
            std::int8_t moveX = 0;
            std::int8_t moveY = 0;
            bool moving = false;
            bool facingLeft = false;
        };

        struct Room {
            std::string code;
            std::array<bool, kMaxPlayersPerRoom> colorUsed{};
            std::array<RoomPlayer, kMaxPlayersPerRoom> players{};
            std::array<bool, kMaxPlayersPerRoom> occupied{};
            std::vector<ChatMessagePacket> chatHistory;
            std::uint32_t tick = 0;
        };

        struct PeerContext {
            std::string roomCode;
            std::uint32_t clientId = 0;
            std::uint8_t colorIndex = 0;
        };

        Room* findRoom(const std::string& code);
        Room& ensureRoom(const std::string& code);
        bool parseRoomCode(const char* code, std::string& out) const;
        std::uint8_t allocateColor(const Room& room) const;
        RoomPlayer* findPlayer(Room& room, std::uint32_t clientId);
        void removePeer(ENetPeer* peer, bool disconnectNow = false);
        void sendJoinResponse(ENetPeer* peer, const JoinResponsePacket& packet);
        void sendRoomSnapshot(Room& room);
        void processJoinRequest(ENetPeer* peer, const JoinRequestPacket& packet);
        void processInputPacket(ENetPeer* peer, const InputPacket& packet);
        void processChatPacket(ENetPeer* peer, const ChatMessagePacket& packet);
        void updateRoom(Room& room, float dt);

        ENetHost* host_ = nullptr;
        bool running_ = false;
        bool enetInitialized_ = false;
        std::uint32_t nextClientId_ = 1;
        std::unordered_map<std::string, Room> rooms_;
    };

} // namespace gameenet
