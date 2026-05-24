#include "enet.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <thread>

namespace gameenet {

    namespace {

        constexpr float kMoveSpeed = 220.0f;
        constexpr std::array<const char*, kMaxPlayersPerRoom> kColorNames = { "red", "blue", "green", "pink" };

        std::string normalizeRoomCode(const char* code) {
            std::string out;
            if (!code) return out;
            for (std::size_t i = 0; code[i] != '\0' && i < kRoomCodeLength; ++i) {
                char c = (char)std::toupper((unsigned char)code[i]);
                if (!std::isalnum((unsigned char)c)) return {};
                out.push_back(c);
            }
            if (out.size() != kRoomCodeLength) return {};
            return out;
        }

        template <typename T>
        ENetPacket* makeReliablePacket(const T& packet) {
            return enet_packet_create(&packet, sizeof(T), ENET_PACKET_FLAG_RELIABLE);
        }

        template <typename T>
        ENetPacket* makeUnreliablePacket(const T& packet) {
            return enet_packet_create(&packet, sizeof(T), 0);
        }

        void copyString(char* dst, std::size_t capacity, const std::string& src) {
            std::memset(dst, 0, capacity);
            std::strncpy(dst, src.c_str(), capacity - 1);
        }

        template <typename T>
        void appendChatLog(std::vector<T>& log, const T& entry, std::size_t maxEntries) {
            log.push_back(entry);
            if (log.size() > maxEntries) {
                log.erase(log.begin());
            }
        }

        void sendChatPacket(ENetPeer* peer, const ChatMessagePacket& packet);

        struct ClientAddress {
            std::string host;
            std::uint16_t port = 0;
        };

        ClientAddress parseClientAddress(const std::string& input, std::uint16_t fallbackPort) {
            ClientAddress result{};
            result.host = input.empty() ? "127.0.0.1" : input;
            result.port = fallbackPort;

            const std::size_t colon = result.host.rfind(':');
            if (colon != std::string::npos && colon + 1 < result.host.size()) {
                bool digits = true;
                for (std::size_t i = colon + 1; i < result.host.size(); ++i) {
                    if (!std::isdigit((unsigned char)result.host[i])) {
                        digits = false;
                        break;
                    }
                }
                if (digits) {
                    int parsedPort = 0;
                    for (std::size_t i = colon + 1; i < result.host.size(); ++i) {
                        parsedPort = parsedPort * 10 + (result.host[i] - '0');
                        if (parsedPort > 65535) {
                            parsedPort = 65535;
                            break;
                        }
                    }
                    result.port = (std::uint16_t)std::clamp(parsedPort, 1, 65535);
                    result.host = result.host.substr(0, colon);
                }
            }

            if (result.host.empty() || result.host == "0.0.0.0" || result.host == "*") {
                result.host = "127.0.0.1";
            }

            return result;
        }

    } // namespace

    ClientNet::~ClientNet() {
        clear();
        if (enetInitialized_) {
            enet_deinitialize();
            enetInitialized_ = false;
        }
    }

    void ClientNet::clear() {
        if (peer_) {
            enet_peer_reset(peer_);
            peer_ = nullptr;
        }
        if (host_) {
            enet_host_destroy(host_);
            host_ = nullptr;
        }
        connected_ = false;
        connecting_ = false;
        joinRequestSent_ = false;
        pendingRoomCode_.clear();
        pendingName_.clear();
        localName_.clear();
        joinInfo_ = {};
        players_.clear();
        chatLog_.clear();
    }

    bool ClientNet::connect(const std::string& host, std::uint16_t port, const std::string& roomCode, const std::string& name, std::string* errorOut) {
        clear();

        if (!enetInitialized_) {
            if (enet_initialize() != 0) {
                lastError_ = "Failed to initialize ENet";
                if (errorOut) *errorOut = lastError_;
                return false;
            }
            enetInitialized_ = true;
        }

        ClientAddress addressSpec = parseClientAddress(host, port);
        const std::string resolvedName = name.empty() ? "Guest" : name;
        localName_ = resolvedName.substr(0, kMaxNameLength);

        std::string normalizedRoom = normalizeRoomCode(roomCode.c_str());
        if (normalizedRoom.empty()) {
            lastError_ = "Room code must be exactly 8 alphanumeric characters";
            if (errorOut) *errorOut = lastError_;
            return false;
        }

        host_ = enet_host_create(nullptr, 1, 2, 0, 0);
        if (!host_) {
            lastError_ = "Failed to create ENet client host";
            if (errorOut) *errorOut = lastError_;
            return false;
        }

        ENetAddress address{};
        if (enet_address_set_host(&address, addressSpec.host.c_str()) != 0) {
            lastError_ = "Could not resolve server host";
            if (errorOut) *errorOut = lastError_;
            clear();
            return false;
        }
        address.port = addressSpec.port;

        peer_ = enet_host_connect(host_, &address, 2, 0);
        if (!peer_) {
            lastError_ = "Could not connect to server";
            if (errorOut) *errorOut = lastError_;
            clear();
            return false;
        }

        pendingRoomCode_ = normalizedRoom;
        pendingName_ = resolvedName.substr(0, kMaxNameLength);
        joinRequestSent_ = false;
        enet_peer_ping_interval(peer_, 1000);
        connecting_ = true;
        lastError_.clear();
        if (errorOut) errorOut->clear();
        return true;
    }

    void ClientNet::disconnect() {
        if (peer_) {
            enet_peer_disconnect_now(peer_, 0);
        }
        clear();
    }

    void ClientNet::sendInput(int moveX, int moveY, bool moving, bool facingLeft) {
        if (!connected_ || !peer_) return;

        InputPacket packet{};
        packet.type = (std::uint8_t)PacketType::InputState;
        packet.moveX = (std::int8_t)std::clamp(moveX, -1, 1);
        packet.moveY = (std::int8_t)std::clamp(moveY, -1, 1);
        packet.moving = moving ? 1 : 0;
        packet.facingLeft = facingLeft ? 1 : 0;

        ENetPacket* enetPacket = makeUnreliablePacket(packet);
        if (!enetPacket) return;

        enet_peer_send(peer_, 0, enetPacket);
    }

    void ClientNet::handleJoinResponse(const JoinResponsePacket& packet) {
        joinInfo_.accepted = packet.accepted != 0;
        joinInfo_.clientId = packet.clientId;
        joinInfo_.colorIndex = packet.colorIndex;
        joinInfo_.roomCode = packet.roomCode;
        joinInfo_.message = packet.message;

        if (!joinInfo_.accepted) {
            lastError_ = joinInfo_.message.empty() ? "Join rejected" : joinInfo_.message;
            connected_ = false;
            connecting_ = false;
            return;
        }

        connected_ = true;
        connecting_ = false;
        lastError_.clear();
        players_.clear();
        chatLog_.clear();
    }

    void ClientNet::handleSnapshot(const SnapshotPacket& packet) {
        players_.clear();
        players_.reserve(packet.playerCount);
        for (std::size_t i = 0; i < packet.playerCount && i < kMaxPlayersPerRoom; ++i) {
            const auto& src = packet.players[i];
            PlayerView view{};
            view.clientId = src.clientId;
            view.colorIndex = src.colorIndex;
            view.x = src.x;
            view.y = src.y;
            view.moving = src.moving != 0;
            view.facingLeft = src.facingLeft != 0;
            view.active = src.active != 0;
            view.name = src.name;
            players_.push_back(std::move(view));
        }
    }

    void ClientNet::handleChatMessage(const ChatMessagePacket& packet) {
        ChatLine line{};
        line.clientId = packet.clientId;
        line.colorIndex = packet.colorIndex;
        line.name = packet.name;
        line.message = packet.message;
        appendChatLog(chatLog_, line, 12);
    }

    void ClientNet::sendPendingJoinRequest() {
        if (!peer_ || joinRequestSent_) return;

        JoinRequestPacket request{};
        request.type = (std::uint8_t)PacketType::JoinRequest;
        copyString(request.roomCode, sizeof(request.roomCode), pendingRoomCode_);
        copyString(request.name, sizeof(request.name), pendingName_);

        ENetPacket* packet = makeReliablePacket(request);
        if (!packet) {
            lastError_ = "Failed to create join packet";
            connecting_ = false;
            connected_ = false;
            return;
        }

        enet_peer_send(peer_, 0, packet);
        enet_host_flush(host_);
        joinRequestSent_ = true;
    }

    void ClientNet::sendChatMessage(const std::string& message) {
        if (!connected_ || !peer_) return;

        std::string trimmed = message;
        if (trimmed.size() > 95) trimmed.resize(95);
        if (trimmed.empty()) return;

        ChatMessagePacket packet{};
        packet.type = (std::uint8_t)PacketType::ChatMessage;
        packet.clientId = joinInfo_.clientId;
        packet.colorIndex = joinInfo_.colorIndex;
        copyString(packet.name, sizeof(packet.name), localName_);
        copyString(packet.message, sizeof(packet.message), trimmed);

        ENetPacket* enetPacket = makeReliablePacket(packet);
        if (!enetPacket) {
            lastError_ = "Failed to create chat packet";
            return;
        }
        enet_peer_send(peer_, 0, enetPacket);
    }

    bool ClientNet::pump(std::string* errorOut) {
        if (!host_) {
            if (errorOut) errorOut->clear();
            return false;
        }

        ENetEvent event{};
        while (enet_host_service(host_, &event, 0) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    sendPendingJoinRequest();
                    break;
                case ENET_EVENT_TYPE_RECEIVE: {
                    if (event.packet->dataLength >= 1) {
                        const auto type = static_cast<PacketType>(event.packet->data[0]);
                        if (type == PacketType::JoinResponse && event.packet->dataLength >= sizeof(JoinResponsePacket)) {
                            JoinResponsePacket packet{};
                            std::memcpy(&packet, event.packet->data, sizeof(packet));
                            handleJoinResponse(packet);
                        } else if (type == PacketType::Snapshot && event.packet->dataLength >= sizeof(SnapshotPacket)) {
                            SnapshotPacket packet{};
                            std::memcpy(&packet, event.packet->data, sizeof(packet));
                            handleSnapshot(packet);
                        } else if (type == PacketType::ChatMessage && event.packet->dataLength >= sizeof(ChatMessagePacket)) {
                            ChatMessagePacket packet{};
                            std::memcpy(&packet, event.packet->data, sizeof(packet));
                            handleChatMessage(packet);
                        }
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                    connected_ = false;
                    connecting_ = false;
                    lastError_ = "Disconnected from server";
                    clear();
                    if (errorOut) *errorOut = lastError_;
                    return false;
                default:
                    break;
            }
        }

        if (errorOut) *errorOut = lastError_;
        return connected_ || connecting_;
    }

    int ClientNet::pingMs() const {
        if (!peer_ || (!connected_ && !connecting_)) return 0;
        return (int)peer_->roundTripTime;
    }

    std::string ClientNet::connectionSummary() const {
        if (connected_) {
            return "connected to " + joinInfo_.roomCode + " as client " + std::to_string(joinInfo_.clientId);
        }
        if (connecting_) {
            return "connecting...";
        }
        return lastError_.empty() ? "offline" : lastError_;
    }

    ServerApp::~ServerApp() {
        stop();
    }

    bool ServerApp::start(const std::string& bindHost, std::uint16_t port, std::string* errorOut) {
        stop();

        if (!enetInitialized_) {
            if (enet_initialize() != 0) {
                if (errorOut) *errorOut = "Failed to initialize ENet";
                return false;
            }
            enetInitialized_ = true;
        }

        ENetAddress address{};
        if (bindHost.empty() || bindHost == "0.0.0.0" || bindHost == "*") {
            address.host = ENET_HOST_ANY;
        } else if (enet_address_set_host_ip(&address, bindHost.c_str()) != 0) {
            if (errorOut) *errorOut = "Could not parse bind address";
            return false;
        }
        address.port = port;

        host_ = enet_host_create(&address, 32, 2, 0, 0);
        if (!host_) {
            if (errorOut) *errorOut = "Failed to create ENet server host";
            return false;
        }

        running_ = true;
        nextClientId_ = 1;
        if (errorOut) errorOut->clear();
        return true;
    }

    void ServerApp::stop() {
        running_ = false;
        rooms_.clear();
        if (host_) {
            enet_host_destroy(host_);
            host_ = nullptr;
        }
        if (enetInitialized_) {
            enet_deinitialize();
            enetInitialized_ = false;
        }
    }

    ServerApp::Room* ServerApp::findRoom(const std::string& code) {
        auto it = rooms_.find(code);
        if (it == rooms_.end()) return nullptr;
        return &it->second;
    }

    ServerApp::Room& ServerApp::ensureRoom(const std::string& code) {
        auto& room = rooms_[code];
        room.code = code;
        return room;
    }

    bool ServerApp::parseRoomCode(const char* code, std::string& out) const {
        out = normalizeRoomCode(code);
        return !out.empty();
    }

    std::uint8_t ServerApp::allocateColor(const Room& room) const {
        for (std::uint8_t i = 0; i < kMaxPlayersPerRoom; ++i) {
            if (!room.colorUsed[i]) return i;
        }
        return 255;
    }

    ServerApp::RoomPlayer* ServerApp::findPlayer(Room& room, std::uint32_t clientId) {
        for (std::size_t i = 0; i < kMaxPlayersPerRoom; ++i) {
            if (room.occupied[i] && room.players[i].clientId == clientId) {
                return &room.players[i];
            }
        }
        return nullptr;
    }

    void ServerApp::removePeer(ENetPeer* peer, bool disconnectNow) {
        if (!peer) return;
        auto* ctx = static_cast<PeerContext*>(peer->data);
        if (!ctx) return;

        auto* room = findRoom(ctx->roomCode);
        if (room) {
            for (std::size_t i = 0; i < kMaxPlayersPerRoom; ++i) {
                if (room->occupied[i] && room->players[i].clientId == ctx->clientId) {
                    room->occupied[i] = false;
                    room->colorUsed[room->players[i].colorIndex] = false;
                    room->players[i] = {};
                    break;
                }
            }
            sendRoomSnapshot(*room);

            bool any = false;
            for (bool occupied : room->occupied) {
                if (occupied) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                rooms_.erase(ctx->roomCode);
            }
        }

        delete ctx;
        peer->data = nullptr;
        if (disconnectNow) {
            enet_peer_reset(peer);
        }
    }

    void ServerApp::sendJoinResponse(ENetPeer* peer, const JoinResponsePacket& packet) {
        ENetPacket* enetPacket = makeReliablePacket(packet);
        if (!enetPacket) return;
        enet_peer_send(peer, 0, enetPacket);
    }

    void ServerApp::sendRoomSnapshot(Room& room) {
        SnapshotPacket packet{};
        packet.type = (std::uint8_t)PacketType::Snapshot;
        copyString(packet.roomCode, sizeof(packet.roomCode), room.code);

        std::uint8_t count = 0;
        for (std::size_t i = 0; i < kMaxPlayersPerRoom; ++i) {
            if (!room.occupied[i]) continue;
            const auto& src = room.players[i];
            auto& dst = packet.players[count++];
            dst.clientId = src.clientId;
            dst.colorIndex = src.colorIndex;
            dst.moving = src.moving ? 1 : 0;
            dst.facingLeft = src.facingLeft ? 1 : 0;
            dst.active = 1;
            dst.x = src.x;
            dst.y = src.y;
            copyString(dst.name, sizeof(dst.name), src.name);
        }
        packet.playerCount = count;
        packet.serverTick = room.tick;

        ENetPacket* enetPacket = makeUnreliablePacket(packet);
        if (!enetPacket) return;

        for (std::size_t i = 0; i < kMaxPlayersPerRoom; ++i) {
            if (room.occupied[i]) {
                enet_peer_send(room.players[i].peer, 0, enetPacket);
            }
        }
        enet_host_flush(host_);
    }

    namespace {

        void sendChatPacket(ENetPeer* peer, const ChatMessagePacket& packet) {
            ENetPacket* enetPacket = makeReliablePacket(packet);
            if (!enetPacket) return;
            enet_peer_send(peer, 0, enetPacket);
        }

    } // namespace

    void ServerApp::processChatPacket(ENetPeer* peer, const ChatMessagePacket& packet) {
        auto* ctx = static_cast<PeerContext*>(peer->data);
        if (!ctx) return;

        auto* room = findRoom(ctx->roomCode);
        if (!room) return;

        auto* player = findPlayer(*room, ctx->clientId);
        if (!player) return;

        std::string text = packet.message;
        if (text.empty()) return;
        if (text.size() > 95) text.resize(95);

        ChatMessagePacket out{};
        out.type = (std::uint8_t)PacketType::ChatMessage;
        out.clientId = player->clientId;
        out.colorIndex = player->colorIndex;
        copyString(out.name, sizeof(out.name), player->name);
        copyString(out.message, sizeof(out.message), text);

        appendChatLog(room->chatHistory, out, 12);
        for (std::size_t i = 0; i < kMaxPlayersPerRoom; ++i) {
            if (room->occupied[i]) {
                sendChatPacket(room->players[i].peer, out);
            }
        }
        enet_host_flush(host_);
    }

    void ServerApp::processJoinRequest(ENetPeer* peer, const JoinRequestPacket& packet) {
        std::string code;
        if (!parseRoomCode(packet.roomCode, code)) {
            JoinResponsePacket response{};
            response.type = (std::uint8_t)PacketType::JoinResponse;
            response.accepted = 0;
            response.reason = (std::uint8_t)JoinRejectReason::InvalidRoomCode;
            copyString(response.message, sizeof(response.message), "Room code must be exactly 8 alphanumeric characters");
            sendJoinResponse(peer, response);
            enet_peer_disconnect_now(peer, 0);
            return;
        }

        std::string name = packet.name;
        if (name.empty()) name = "Guest";
        if (name.size() > kMaxNameLength) name.resize(kMaxNameLength);

        Room& room = ensureRoom(code);
        std::uint8_t color = allocateColor(room);
        if (color == 255) {
            JoinResponsePacket response{};
            response.type = (std::uint8_t)PacketType::JoinResponse;
            response.accepted = 0;
            response.reason = (std::uint8_t)JoinRejectReason::RoomFull;
            copyString(response.message, sizeof(response.message), "Room is full");
            sendJoinResponse(peer, response);
            enet_peer_disconnect_now(peer, 0);
            return;
        }

        std::uint32_t clientId = nextClientId_++;
        std::size_t slot = color;
        room.colorUsed[color] = true;
        room.occupied[slot] = true;
        room.players[slot] = {};
        room.players[slot].peer = peer;
        room.players[slot].clientId = clientId;
        room.players[slot].colorIndex = color;
        room.players[slot].name = name;
        room.players[slot].x = 80.0f + (float)slot * 120.0f;
        room.players[slot].y = 80.0f + (float)slot * 40.0f;
        room.players[slot].facingLeft = false;
        room.players[slot].moving = false;

        auto* ctx = new PeerContext{};
        ctx->roomCode = room.code;
        ctx->clientId = clientId;
        ctx->colorIndex = color;
        peer->data = ctx;

        JoinResponsePacket response{};
        response.type = (std::uint8_t)PacketType::JoinResponse;
        response.accepted = 1;
        response.reason = (std::uint8_t)JoinRejectReason::None;
        response.playerCount = (std::uint8_t)std::count(room.occupied.begin(), room.occupied.end(), true);
        response.clientId = clientId;
        response.colorIndex = color;
        copyString(response.roomCode, sizeof(response.roomCode), room.code);
        copyString(response.message, sizeof(response.message), std::string("Assigned ") + kColorNames[color] + " room slot");
        sendJoinResponse(peer, response);
        sendRoomSnapshot(room);
        for (const auto& chat : room.chatHistory) {
            sendChatPacket(peer, chat);
        }
    }

    void ServerApp::processInputPacket(ENetPeer* peer, const InputPacket& packet) {
        auto* ctx = static_cast<PeerContext*>(peer->data);
        if (!ctx) return;

        auto* room = findRoom(ctx->roomCode);
        if (!room) return;

        RoomPlayer* player = findPlayer(*room, ctx->clientId);
        if (!player) return;

        player->moveX = std::clamp((int)packet.moveX, -1, 1);
        player->moveY = std::clamp((int)packet.moveY, -1, 1);
        player->moving = packet.moving != 0;
        player->facingLeft = packet.facingLeft != 0;
    }

    void ServerApp::updateRoom(Room& room, float dt) {
        for (std::size_t i = 0; i < kMaxPlayersPerRoom; ++i) {
            if (!room.occupied[i]) continue;
            auto& player = room.players[i];
            const float dx = (float)player.moveX;
            const float dy = (float)player.moveY;
            const float length = sqrtf(dx * dx + dy * dy);
            float nx = dx;
            float ny = dy;
            if (length > 0.0f) {
                nx /= length;
                ny /= length;
            }

            player.x += nx * kMoveSpeed * dt;
            player.y += ny * kMoveSpeed * dt;
            if (player.moveX != 0) {
                player.facingLeft = player.moveX < 0;
            }
            player.moving = player.moveX != 0 || player.moveY != 0;
        }
        ++room.tick;
    }

    void ServerApp::run() {
        if (!host_) return;

        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        const float fixedStep = 1.0f / 60.0f;
        float accumulator = 0.0f;

        while (running_) {
            ENetEvent event{};
            while (enet_host_service(host_, &event, 0) > 0) {
                switch (event.type) {
                    case ENET_EVENT_TYPE_CONNECT:
                        break;
                    case ENET_EVENT_TYPE_RECEIVE: {
                        if (event.packet->dataLength >= 1) {
                            const auto type = static_cast<PacketType>(event.packet->data[0]);
                            if (type == PacketType::JoinRequest && event.packet->dataLength >= sizeof(JoinRequestPacket)) {
                                JoinRequestPacket packet{};
                                std::memcpy(&packet, event.packet->data, sizeof(packet));
                                processJoinRequest(event.peer, packet);
                            } else if (type == PacketType::InputState && event.packet->dataLength >= sizeof(InputPacket)) {
                                InputPacket packet{};
                                std::memcpy(&packet, event.packet->data, sizeof(packet));
                                processInputPacket(event.peer, packet);
                            } else if (type == PacketType::ChatMessage && event.packet->dataLength >= sizeof(ChatMessagePacket)) {
                                ChatMessagePacket packet{};
                                std::memcpy(&packet, event.packet->data, sizeof(packet));
                                processChatPacket(event.peer, packet);
                            }
                        }
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    case ENET_EVENT_TYPE_DISCONNECT:
                        removePeer(event.peer, false);
                        break;
                    default:
                        break;
                }
            }

            auto now = clock::now();
            float delta = std::chrono::duration<float>(now - last).count();
            last = now;
            accumulator += delta;

            while (accumulator >= fixedStep) {
                for (auto& [code, room] : rooms_) {
                    (void)code;
                    updateRoom(room, fixedStep);
                    sendRoomSnapshot(room);
                }
                accumulator -= fixedStep;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        stop();
    }

} // namespace gameenet
